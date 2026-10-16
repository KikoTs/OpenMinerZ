#include "openminerz/screens.hpp"

#include "openminerz/remastered_layout.hpp"
#include "openminerz/retail_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace openminerz::screens
{
namespace
{

constexpr Color white{255U, 255U, 255U, 255U};
constexpr Color black{0U, 0U, 0U, 255U};
constexpr Color red{255U, 0U, 0U, 255U};
// new Color(0.25f, 0.25f, 0.25f, 0.5f): an uncraftable recipe or a missing ingredient.
constexpr Color dimmed{64U, 64U, 64U, 128U};
// CMZColors.
constexpr Color menu_aqua{53U, 170U, 253U, 255U};
constexpr Color menu_blue{100U, 158U, 198U, 255U};
constexpr Color menu_orange{226U, 106U, 61U, 255U};
// InventoryItem.Draw2D's wear bar.
constexpr Color wear_green{67U, 188U, 0U, 255U};
// XNA's named colours used by the close box and FrameButtonControl.
constexpr Color gray{128U, 128U, 128U, 255U};
constexpr Color light_gray{211U, 211U, 211U, 255U};
constexpr Color dark_gray{169U, 169U, 169U, 255U};
constexpr Color dim_gray{105U, 105U, 105U, 255U};

// 1.6.3.8's fixed 1280x720 coordinates.
constexpr IntRect classic_background{235, 59, 810, 602};
constexpr int classic_cell = 59;
constexpr int classic_grid_x = 404;
constexpr int classic_backpack_y = 334;
constexpr int classic_tray_y = 584;
constexpr int classic_crate_y = 83; // background top + 24
// CraftingUIScreen._gridRect: a click here returns to the inventory screen.
constexpr IntRect classic_crafting_grid{400, 330, 480, 320};
constexpr int strip_pitch = 77;

std::string number(float value)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
    std::string text{buffer};
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    if (text == "-0")
        text = "0";
    return text;
}

std::string rgba(Color color)
{
    return std::to_string(color.r) + "," + std::to_string(color.g) + "," + std::to_string(color.b) +
           "," + std::to_string(color.a);
}

Draw sprite(Sprite id, float x, float y, float width, float height, Color color = white)
{
    Draw draw;
    draw.kind = Draw::Kind::sprite;
    draw.sprite = id;
    draw.x = x;
    draw.y = y;
    draw.width = width;
    draw.height = height;
    draw.color = color;
    return draw;
}

Draw sprite(Sprite id, IntRect rect, Color color = white)
{
    return sprite(id, static_cast<float>(rect.x), static_cast<float>(rect.y),
                  static_cast<float>(rect.width), static_cast<float>(rect.height), color);
}

Draw text(Font font, std::string value, float x, float y, Color color, int outline = 0,
          float scale = 1.0F, Color outline_color = black)
{
    Draw draw;
    draw.kind = Draw::Kind::text;
    draw.font = font;
    draw.text = std::move(value);
    draw.x = x;
    draw.y = y;
    draw.color = color;
    draw.outline = outline;
    draw.outline_color = outline_color;
    draw.scale = scale;
    return draw;
}

Draw marker(Draw::Kind kind)
{
    Draw draw;
    draw.kind = kind;
    return draw;
}

// Globalization.Strings of the build whose layout is drawn.
std::string phrase(RetailString id, bool modern)
{
    return std::string{retail_text(id, modern ? RetailGeneration::modern : RetailGeneration::classic)};
}

// CraftingScreen's Tier2Item titles: Strings.Ores ... Strings.Doors.
RetailString type_title(RecipeType type) noexcept
{
    switch (type)
    {
    case RecipeType::ores:
        return RetailString::Ores;
    case RecipeType::components:
        return RetailString::Components;
    case RecipeType::pickaxes:
        return RetailString::Pickaxes;
    case RecipeType::spades:
        return RetailString::Spades;
    case RecipeType::axes:
        return RetailString::Axes;
    case RecipeType::special_tools:
        return RetailString::Special;
    case RecipeType::ammo:
        return RetailString::Ammo;
    case RecipeType::knives:
        return RetailString::Knives;
    case RecipeType::pistols:
        return RetailString::Pistols;
    case RecipeType::shotguns:
        return RetailString::Shotguns;
    case RecipeType::rifles:
        return RetailString::Rifles;
    case RecipeType::assault_rifles:
        return RetailString::Assault_Rifles;
    case RecipeType::smgs:
        return RetailString::SMG_s;
    case RecipeType::lmgs:
        return RetailString::LMG_s;
    case RecipeType::rpg:
        return RetailString::RPG;
    case RecipeType::laser_swords:
        return RetailString::Laser_Swords;
    case RecipeType::walls:
        return RetailString::Walls;
    case RecipeType::doors:
        return RetailString::Doors;
    case RecipeType::explosives:
        return RetailString::Explosives;
    case RecipeType::containers:
        return RetailString::Containers;
    case RecipeType::spawn_points:
        return RetailString::Spawn_Points;
    default:
        return RetailString::Other;
    }
}

// InventoryItem.Split(): a NEW item of half the stack, rounded down.
InventoryStack fresh(const InventoryStack &source, std::uint16_t count) noexcept
{
    InventoryStack result{source.item, count};
    result.block = source.block;
    result.mod_item = source.mod_item; // Modding, gameplay area: a split keeps its identity
    return result;
}

std::uint16_t limit_of(const Host &host, InventoryItemId item) noexcept
{
    return item_stack_limit(item, host.inventory().profile());
}

// "Craft all" as the WORLD's own crafting screen means it, whichever style is
// showing that world. 1.9.9.8 ignores it in Creative (Tier2Item.CheckInput:
// its CanCraft is always true there, so "while it can be made" would never
// end); 1.6.3.8 hands it to Craft as it is.
bool craft_every(const Host &host, bool all) noexcept
{
    return all && !(host.inventory().profile() == InventoryProfile::modern && host.creative());
}

// InventoryItem.Stack(item): move as much of `source` as fits into `target`.
void stack_into(const Host &host, InventoryStack &target, InventoryStack &source) noexcept
{
    const std::uint16_t limit = limit_of(host, target.item);
    if (!target.same_kind(source) || target.count >= limit)
        return;
    const auto moved = std::min<std::uint16_t>(source.count, static_cast<std::uint16_t>(limit - target.count));
    target.count = static_cast<std::uint16_t>(target.count + moved);
    source.count = static_cast<std::uint16_t>(source.count - moved);
}

std::string upper(std::string_view value)
{
    std::string result{value};
    for (char &c : result)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return result;
}

// TextRegionElement.ProcessText: break at separators once the text measured so
// far (times the screen scale) is wider than the region.
std::string wrap(std::string_view value, Font font, float width, double scale,
                 const TextMetrics &metrics, int &lines)
{
    std::string out;
    std::string current;
    std::string good;
    lines = 1;
    const auto too_wide = [&]() {
        return static_cast<double>(metrics.measure(font, current).width) * scale > static_cast<double>(width);
    };
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        const char c = value[index];
        current.push_back(c);
        if (c == ' ')
        {
            if (too_wide())
            {
                out += good;
                out.push_back('\n');
                ++lines;
                current.erase(0, good.size());
            }
            good = current;
        }
    }
    if (too_wide())
    {
        out += good;
        out.push_back('\n');
        ++lines;
        current.erase(0, good.size());
    }
    out += current;
    return out;
}

// The original's (int) casts. Its 32-bit JIT evaluates a float expression on
// the x87 stack, in double precision, and only rounds to float when the value
// is stored - so 243f * (1200f / 720f) truncates to 404, not to the 405 that
// float arithmetic gives. Expressions below are therefore written in double
// (s is the float scale widened), with float casts where the original
// stores into a float or a Vector2.
int truncate(double value) noexcept { return static_cast<int>(value); }

} // namespace

std::string_view sprite_sheet_name(Sprite id) noexcept
{
    switch (id)
    {
    case Sprite::block_ui_back:
        return "BlockUIBack";
    case Sprite::selector:
        return "Selector";
    case Sprite::single_grid:
        return "SingleGrid";
    case Sprite::inventory_grid:
        return "InventoryGrid";
    case Sprite::hud_grid:
        return "HudGrid";
    case Sprite::tier2_back:
        return "Tier2Back";
    case Sprite::craft_selector:
        return "CraftSelector";
    case Sprite::materials_icon:
        return "MaterialsIcon";
    case Sprite::tools_icon:
        return "ToolsIcon";
    case Sprite::weapons_icon:
        return "WeaponsIcon";
    case Sprite::structures_icon:
        return "StructuresIcon";
    case Sprite::frame_top_left:
        return "CtrlFrame_tl";
    case Sprite::frame_top_right:
        return "CtrlFrame_tr";
    case Sprite::frame_bottom_left:
        return "CtrlFrame_bl";
    case Sprite::frame_bottom_right:
        return "CtrlFrame_br";
    case Sprite::frame_top:
        return "CtrlFrame_t";
    case Sprite::frame_bottom:
        return "CtrlFrame_b";
    case Sprite::frame_left:
        return "CtrlFrame_l";
    case Sprite::frame_right:
        return "CtrlFrame_r";
    case Sprite::frame_center:
        return "CtrlFrame_c";
    default:
        return {};
    }
}

Size sprite_size(UiStyle style, Sprite id) noexcept
{
    const bool modern = style == UiStyle::modern;
    switch (id)
    {
    case Sprite::block_ui_back:
        return modern ? Size{898, 586} : Size{810, 602};
    case Sprite::selector:
        return modern ? Size{76, 76} : Size{68, 68};
    case Sprite::single_grid:
        return {76, 76};
    case Sprite::inventory_grid:
        return modern ? Size{264, 520} : Size{491, 254};
    case Sprite::hud_grid:
        return modern ? Size{481, 68} : Size{485, 72};
    case Sprite::tier2_back:
        return modern ? Size{104, 30} : Size{};
    case Sprite::craft_selector:
        return modern ? Size{163, 36} : Size{};
    case Sprite::materials_icon:
    case Sprite::tools_icon:
    case Sprite::weapons_icon:
    case Sprite::structures_icon:
        return modern ? Size{35, 35} : Size{};
    case Sprite::frame_top_left:
    case Sprite::frame_top_right:
    case Sprite::frame_bottom_left:
    case Sprite::frame_bottom_right:
        return {2, 2};
    case Sprite::frame_top:
    case Sprite::frame_bottom:
        return {1, 2};
    case Sprite::frame_left:
    case Sprite::frame_right:
        return {2, 1};
    case Sprite::frame_center:
        return {1, 1};
    default:
        return {};
    }
}

std::string_view font_asset_name(Font font) noexcept
{
    switch (font)
    {
    case Font::small:
        return "SmallFont";
    case Font::medium:
        return "MedFont";
    case Font::myriad_medium:
        return "MyriadMedium";
    default:
        return {};
    }
}

std::string_view cue_name(Cue cue) noexcept
{
    switch (cue)
    {
    case Cue::click:
        return "Click";
    case Cue::error:
        return "Error";
    case Cue::craft:
        return "craft";
    case Cue::drop_item:
        return "dropitem";
    }
    return {};
}

std::string record(const Draw &draw)
{
    switch (draw.kind)
    {
    case Draw::Kind::hud_stats:
        return "E hud DrawPlayerStats";
    case Draw::Kind::hud_distance:
        return "E hud DrawDistanceStr";
    case Draw::Kind::text:
    {
        std::string line = "T font=" + std::string{font_asset_name(draw.font)};
        line += " pos=" + number(draw.x) + "," + number(draw.y);
        line += " scale=" + number(draw.scale) + "," + number(draw.scale);
        line += " rgba=" + rgba(draw.color);
        if (draw.outline != 0)
            line += " outline=" + std::to_string(draw.outline) + " orgba=" + rgba(draw.outline_color);
        line += " text=\"";
        for (const char c : draw.text)
        {
            if (c == '"' || c == '\\')
                line.push_back('\\');
            if (c == '\n')
                line += "\\n";
            else
                line.push_back(c);
        }
        line.push_back('"');
        return line;
    }
    case Draw::Kind::sprite:
        break;
    }
    std::string name;
    if (draw.sprite == Sprite::solid)
        name = "@white";
    else if (draw.sprite == Sprite::item_icon)
        name = "@item:" + std::to_string(draw.icon);
    else if (draw.sprite == Sprite::item_icon_large)
        name = "@item-large:" + std::to_string(draw.icon);
    else
        name = std::string{sprite_sheet_name(draw.sprite)};
    return "S name=" + name + " dst=" + number(draw.x) + "," + number(draw.y) + "," +
           number(draw.width) + "," + number(draw.height) + " rgba=" + rgba(draw.color);
}

IntRect classic_buffer_destination(int window_width, int window_height) noexcept
{
    if (window_width <= 0 || window_height <= 0)
        return {};
    // DNAGame.Draw: float aspect comparison, integer rectangle.
    const float aspect = static_cast<float>(window_width) / static_cast<float>(window_height);
    const float buffer_aspect =
        static_cast<float>(classic_buffer_width) / static_cast<float>(classic_buffer_height);
    if (aspect > buffer_aspect)
    {
        const int width = window_height * classic_buffer_width / classic_buffer_height;
        return {(window_width - width) / 2, 0, width, window_height};
    }
    const int height = window_width * classic_buffer_height / classic_buffer_width;
    return {0, (window_height - height) / 2, window_width, height};
}

Point classic_window_to_buffer(Point window, IntRect destination) noexcept
{
    if (destination.width == 0 || destination.height == 0)
        return {};
    return {(window.x - destination.x) * classic_buffer_width / destination.width,
            (window.y - destination.y) * classic_buffer_height / destination.height};
}

int retail_icon(InventoryItemId item, UiStyle style) noexcept
{
    const auto value = static_cast<int>(item);
    const InventoryProfile profile =
        style == UiStyle::modern ? InventoryProfile::modern : InventoryProfile::classic;
    if (!item_registered(item, profile))
        return -1;
    if (value <= static_cast<int>(InventoryItemId::blood_stone_lmg))
        return value;
    // 1.9.9.8's own ids continue the shared enum at 123.
    const int first = static_cast<int>(InventoryItemId::modern_chainsaw_1);
    return value >= first ? value - first + 123 : -1;
}

RecipeType recipe_type_for(const CraftingRecipe &recipe, InventoryProfile world) noexcept
{
    if (world == InventoryProfile::modern)
        return recipe.type;
    for (const CraftingRecipe &modern : crafting_recipes(InventoryProfile::modern))
        if (modern.result == recipe.result && recipe_listed(modern))
            return modern.type;
    // Results 1.9.9.8 no longer has a listed recipe for: file them with their
    // own kind so nothing is unreachable.
    if (gun_stats(recipe.result) != nullptr)
        return RecipeType::assault_rifles;
    if (is_block_item(recipe.result))
        return RecipeType::other_structure;
    switch (item_category(recipe.result))
    {
    case 6:
        return RecipeType::ammo;
    case 4:
        return RecipeType::components;
    default:
        return RecipeType::special_tools;
    }
}

// ---------------------------------------------------------------- geometry

struct Session::Geometry final
{
    bool modern{};
    // Remastered: slots are remastered_layout.hpp's grid, in panel units.
    bool remastered{};
    double s{1.0}; // the float ScaleFactor.Y, widened
    IntRect background{};
    float inventory_x{};
    float inventory_y{};
    float crate_x{};
    float crate_y{};
    float spacing{static_cast<float>(classic_cell)};
    int cell{classic_cell};
    int columns{8};
    std::array<IntRect, 2> trays{};
    std::size_t visible_tray{};
    std::size_t tray_count{1U};
    bool crate{};

    // Where the item is drawn.
    [[nodiscard]] std::optional<IntRect> draw_rect(std::size_t slot) const noexcept
    {
        if (remastered)
        {
            namespace rm = openminerz::remastered;
            const auto grid_cell = [](int left, int top, std::size_t index) {
                return IntRect{left + static_cast<int>(index % 8U) * rm::slot_pitch,
                               top + static_cast<int>(index / 8U) * rm::slot_pitch, rm::slot_size,
                               rm::slot_size};
            };
            if (slot >= crate_base)
            {
                if (!crate || slot >= crate_base + crate_slots)
                    return std::nullopt;
                return grid_cell(rm::left_x, rm::grid_y, slot - crate_base);
            }
            if (slot < backpack_slots)
                return grid_cell(rm::right_x, rm::grid_y, slot);
            // One row per tray, both always on screen in a two-tray world.
            if ((slot - tray_base) / 8U >= tray_count)
                return std::nullopt;
            return grid_cell(rm::right_x, rm::tray_y, slot - tray_base);
        }
        if (slot >= crate_base)
        {
            if (!crate || slot >= crate_base + crate_slots)
                return std::nullopt;
            const auto index = static_cast<int>(slot - crate_base);
            return IntRect{truncate(crate_x + spacing * static_cast<float>(index % columns)),
                           truncate(crate_y + spacing * static_cast<float>(index / columns)), cell, cell};
        }
        if (slot < backpack_slots)
        {
            const auto index = static_cast<int>(slot);
            return IntRect{truncate(inventory_x + spacing * static_cast<float>(index % columns)),
                           truncate(inventory_y + spacing * static_cast<float>(index / columns)), cell, cell};
        }
        const std::size_t tray = (slot - tray_base) / 8U;
        const auto column = static_cast<int>((slot - tray_base) % 8U);
        if (tray >= tray_count)
            return std::nullopt;
        if (!modern)
        {
            if (tray != visible_tray)
                return std::nullopt;
            return IntRect{classic_grid_x + classic_cell * column, classic_tray_y, classic_cell, classic_cell};
        }
        const IntRect &grid = trays[tray];
        // new Vector2(itemSpacing * i + gridRect.Left + 2f * scale, gridRect.Top + 2f * scale)
        return IntRect{truncate(static_cast<float>(static_cast<double>(spacing) * column + grid.x + 2.0 * s)),
                       truncate(static_cast<float>(grid.y + 2.0 * s)), cell, cell};
    }

    // What the pointer is tested against: 1.9.9.8's backpack and crate cells
    // are (int)(59 * scale) wide, five pixels narrower than the icon.
    [[nodiscard]] std::optional<IntRect> hit_rect(std::size_t slot) const noexcept
    {
        auto rect = draw_rect(slot);
        if (rect && remastered)
        {
            // Half the gap on every side, so neighbouring slots meet and the
            // pointer is always on one of them while it crosses a grid.
            const int half = openminerz::remastered::slot_gap / 2;
            *rect = {rect->x - half, rect->y - half, openminerz::remastered::slot_pitch,
                     openminerz::remastered::slot_pitch};
            return rect;
        }
        if (rect && modern && (slot < backpack_slots || slot >= crate_base))
            rect->width = rect->height = truncate(spacing);
        return rect;
    }

    // Where the Selector sprite goes.
    [[nodiscard]] IntRect selector_rect(std::size_t slot, Size selector) const noexcept
    {
        const IntRect rect = draw_rect(slot).value_or(IntRect{});
        if (!modern)
            return {rect.x - 4, rect.y - 4, selector.width, selector.height};
        if (slot >= tray_base && slot < crate_base)
        {
            const std::size_t tray = std::min<std::size_t>((slot - tray_base) / 8U, 1U);
            const auto column = static_cast<double>((slot - tray_base) % 8U);
            const IntRect &grid = trays[tray];
            return {grid.x + truncate(7.0 * s + static_cast<double>(spacing) * column),
                    truncate(grid.y + 7.0 * s), cell, cell};
        }
        return rect;
    }
};

bool Session::modern_layout() const noexcept
{
    const Layout current = layout();
    return current == Layout::modern_inventory || current == Layout::modern_crate;
}

bool Session::crafting_in_inventory() const noexcept
{
    // Only 1.6.3.8 has a crafting screen of its own.
    return style_ != UiStyle::classic;
}

Layout Session::layout() const noexcept
{
    if (style_ == UiStyle::remastered)
        return screen_ == ScreenId::crate ? Layout::remastered_crate : Layout::remastered_inventory;
    const bool modern_ui = style_ == UiStyle::modern;
    switch (screen_)
    {
    case ScreenId::crate:
        return modern_ui ? Layout::modern_crate : Layout::classic_crate;
    case ScreenId::crafting:
        return modern_ui ? Layout::modern_inventory : Layout::classic_crafting;
    default:
        return modern_ui ? Layout::modern_inventory : Layout::classic_inventory;
    }
}

Session::Geometry Session::geometry() const noexcept
{
    Geometry g;
    const Layout current = layout();
    g.modern = current == Layout::modern_inventory || current == Layout::modern_crate;
    g.crate = screen_ == ScreenId::crate;
    if (remastered_layout())
    {
        g.remastered = true;
        g.tray_count = static_cast<std::size_t>(rm_trays_);
        g.background = {0, 0, openminerz::remastered::panel_width,
                        openminerz::remastered::panel_height(rm_trays_)};
        return g;
    }
    if (!g.modern)
    {
        g.background = classic_background;
        g.inventory_x = static_cast<float>(classic_grid_x);
        g.inventory_y = static_cast<float>(classic_backpack_y);
        g.crate_x = static_cast<float>(classic_grid_x);
        g.crate_y = static_cast<float>(classic_crate_y);
        return g;
    }
    // Screen.Adjuster.ScaleFactor.Y; every length below is the original's
    // expression, in its float arithmetic and with its truncations.
    g.s = static_cast<float>(height_) / 720.0F;
    g.columns = 4;
    g.spacing = static_cast<float>(59.0 * g.s);
    g.cell = truncate(64.0 * g.s);
    const Size hud = sprite_size(UiStyle::modern, Sprite::hud_grid);
    const Size tray{truncate(hud.width * g.s), truncate(hud.height * g.s)};
    const int tray_y = height_ - tray.height - truncate(5.0 * g.s);
    g.trays[0] = {width_ / 2 - tray.width, tray_y, tray.width, tray.height};
    g.trays[1] = {width_ / 2, tray_y, tray.width, tray.height};
    if (current == Layout::modern_inventory)
    {
        const Size art = sprite_size(UiStyle::modern, Sprite::block_ui_back);
        const Size size{truncate(art.width * g.s), truncate(art.height * g.s)};
        g.background = {width_ / 2 - size.width / 2, truncate(55.0 * g.s), size.width, size.height};
        g.inventory_x = static_cast<float>(g.background.x + 646.0 * g.s);
        g.inventory_y = static_cast<float>(g.background.y + 96.0 * g.s);
    }
    else
    {
        const Size grid = sprite_size(UiStyle::modern, Sprite::inventory_grid);
        // new Vector2(grid.Width * 2 + 16, grid.Height) * scale
        const auto width = static_cast<float>((grid.width * 2 + 16) * g.s);
        const auto height = static_cast<float>(grid.height * g.s);
        g.background = {truncate(width_ / 2 - static_cast<double>(width) / 2.0),
                        truncate(height_ / 2 - static_cast<double>(height) / 2.0), truncate(width),
                        truncate(height)};
        g.inventory_x = static_cast<float>(g.background.x + 299.0 * g.s);
        g.inventory_y = static_cast<float>(g.background.y + 33.0 * g.s);
        g.crate_x = static_cast<float>(g.background.x + 17.0 * g.s);
        g.crate_y = g.inventory_y;
    }
    return g;
}

Session::RemasteredView Session::remastered_view() const noexcept
{
    namespace rm = openminerz::remastered;
    // The whole panel inside the screen margins, with a little air around it.
    const auto panel_width = static_cast<float>(rm::panel_width);
    const auto panel_height = static_cast<float>(rm::panel_height(rm_trays_));
    const rm::Canvas canvas =
        rm::canvas(width_, height_, ui_scale_, safe_area_, panel_width + 24.0F, panel_height + 24.0F);
    RemasteredView view;
    view.scale = canvas.scale;
    view.trays = rm_trays_;
    // Whole pixels, so every slot edge lands on one.
    view.origin_x = std::floor((static_cast<float>(width_) - panel_width * canvas.scale) * 0.5F);
    view.origin_y = std::floor((static_cast<float>(height_) - panel_height * canvas.scale) * 0.5F);
    return view;
}

Point Session::to_window(Point point) const noexcept
{
    if (remastered_layout())
    {
        // The middle of the unit, like the Classic buffer pixel below.
        const RemasteredView view = remastered_view();
        return {static_cast<int>(std::floor(view.origin_x + (static_cast<float>(point.x) + 0.5F) * view.scale)),
                static_cast<int>(std::floor(view.origin_y + (static_cast<float>(point.y) + 0.5F) * view.scale))};
    }
    if (modern_layout())
        return point;
    // A window pixel inside the block the buffer pixel covers (its centre).
    const IntRect destination = classic_buffer_destination(width_, height_);
    return {static_cast<int>(std::floor((static_cast<double>(point.x) + 0.5) * destination.width / classic_buffer_width)) + destination.x,
            static_cast<int>(std::floor((static_cast<double>(point.y) + 0.5) * destination.height / classic_buffer_height)) + destination.y};
}

Point Session::to_layout(Point window) const noexcept
{
    if (remastered_layout())
    {
        const RemasteredView view = remastered_view();
        return {static_cast<int>(std::floor((static_cast<float>(window.x) - view.origin_x) / view.scale)),
                static_cast<int>(std::floor((static_cast<float>(window.y) - view.origin_y) / view.scale))};
    }
    if (modern_layout())
        return window;
    return classic_window_to_buffer(window, classic_buffer_destination(width_, height_));
}

std::size_t Session::tray_count(const Host &host) const noexcept
{
    return host.inventory().profile() == InventoryProfile::modern ? 2U : 1U;
}

void Session::configure(UiStyle style, int width, int height, float ui_scale, float safe_area) noexcept
{
    // One style for every screen. `automatic` is resolved by the caller; if it
    // arrives anyway it is a retail look, as the setting says.
    style_ = style == UiStyle::remastered ? UiStyle::remastered
             : style == UiStyle::modern   ? UiStyle::modern
                                          : UiStyle::classic;
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    ui_scale_ = ui_scale;
    safe_area_ = safe_area;
}

bool Session::recipe_tree_shown() const noexcept
{
    // CraftingScreen always has its recipe tree: 1.9.9.8's inventory screen
    // IS its crafting screen.
    return layout() == Layout::modern_inventory;
}

// ------------------------------------------------------------------- slots

PlayerInventory::Slot *Session::slot(Host &host, std::size_t index) const noexcept
{
    if (index >= crate_base)
        return index < crate_base + crate_slots ? host.crate_slot(index - crate_base) : nullptr;
    PlayerInventory &inventory = host.inventory();
    if (index < backpack_slots)
        return &inventory.storage()[index];
    const std::size_t tray = (index - tray_base) / 8U;
    if (tray >= tray_count(host))
        return nullptr;
    return &inventory.tray(tray)[(index - tray_base) % 8U];
}

const PlayerInventory::Slot *Session::slot(const Host &host, std::size_t index) const noexcept
{
    if (index >= crate_base)
        return index < crate_base + crate_slots ? host.crate_slot(index - crate_base) : nullptr;
    const PlayerInventory &inventory = host.inventory();
    if (index < backpack_slots)
        return &inventory.storage()[index];
    const std::size_t tray = (index - tray_base) / 8U;
    if (tray >= tray_count(host))
        return nullptr;
    return &inventory.tray(tray)[(index - tray_base) % 8U];
}

void Session::write(Host &host, std::size_t index, PlayerInventory::Slot value) const
{
    PlayerInventory::Slot *target = slot(host, index);
    if (target == nullptr)
        return;
    if (value.has_value() && value->count == 0U)
        value.reset();
    *target = std::move(value);
    if (index >= crate_base)
        host.crate_changed(index - crate_base);
}

bool Session::selected_locked(const Host &host) const noexcept
{
    return selected_ >= crate_base && host.crate_slot_locked(selected_ - crate_base);
}

std::optional<std::size_t> Session::slot_at(const Host &host, Point point) const noexcept
{
    Geometry g = geometry();
    g.visible_tray = host.inventory().current_tray_index();
    g.tray_count = tray_count(host);
    // InventoryHitTest first, then CrateHitTest.
    for (std::size_t index = 0; index < tray_base + 8U * g.tray_count; ++index)
        if (const auto rect = g.hit_rect(index); rect && rect->contains(point))
            return index;
    if (g.crate)
        for (std::size_t index = crate_base; index < crate_base + crate_slots; ++index)
            if (const auto rect = g.hit_rect(index); rect && rect->contains(point))
                return index;
    return std::nullopt;
}

bool Session::selector_visible() const noexcept
{
    switch (layout())
    {
    case Layout::modern_inventory:
        return hit_;
    case Layout::classic_crafting:
        return false;
    case Layout::remastered_inventory:
    case Layout::remastered_crate:
        // With the pointer: the slot under it. With keys or the pad: the slot
        // the cursor is on, unless the cursor is in the recipe list.
        return pointer_active_ ? hit_ : !rm_recipes_focused_;
    default:
        return !pointer_active_ || hit_;
    }
}

IntRect Session::panel() const noexcept { return geometry().background; }

std::optional<IntRect> Session::slot_rect(std::size_t index) const noexcept
{
    Geometry g = geometry();
    // (Remastered shows exactly the trays the world has, both at once.)
    if (!g.remastered)
        g.tray_count = 2U;
    g.visible_tray = index >= tray_base && index < crate_base ? (index - tray_base) / 8U : 0U;
    return g.hit_rect(index);
}

std::optional<IntRect> Session::close_rect() const noexcept
{
    const Layout current = layout();
    if (current == Layout::remastered_inventory || current == Layout::remastered_crate)
        return IntRect{openminerz::remastered::panel_width - openminerz::remastered::panel_padding - 28, 12,
                       28, 28};
    if (current != Layout::classic_inventory && current != Layout::classic_crafting)
        return std::nullopt;
    return IntRect{classic_background.right() - 43, classic_background.y + 15, 25, 25};
}

std::optional<IntRect> Session::button_rect() const noexcept
{
    switch (layout())
    {
    case Layout::classic_inventory:
        return IntRect{classic_grid_x, 160, 200, 40};
    case Layout::classic_crafting:
        return IntRect{400, 160, 150, 40};
    default:
        // 1.9.9.8's screens have no frame button; Remastered's buttons are
        // its own (remastered_craft_rect and friends).
        return std::nullopt;
    }
}

// ----------------------------------------------------------------- recipes

std::vector<std::size_t> Session::strip_recipes(const Host &host) const
{
    const PlayerInventory &inventory = host.inventory();
    const auto &book = host.recipes();
    std::vector<std::size_t> list;
    std::vector<bool> listed(book.size(), false);
    // A Modern world has no discovery: every recipe a tab lists is known.
    const bool everything = inventory.profile() == InventoryProfile::modern;
    for (const bool wants_craftable : {true, false})
        for (std::size_t index = 0; index < book.size(); ++index)
        {
            if (listed[index] || !recipe_listed(book[index]))
                continue;
            if (!everything && !inventory.recipe_discovered(book[index]))
                continue;
            if (host.can_craft(book[index]) != wants_craftable)
                continue;
            listed[index] = true;
            list.push_back(index);
        }
    // PlayerInventory.DiscoverRecipies: then whatever makes an ingredient of a
    // listed recipe, and Receipe.CookBook[0] if nothing is known at all.
    for (std::size_t cursor = 0; cursor < list.size(); ++cursor)
        for (const CraftingIngredient &ingredient : book[list[cursor]].ingredients)
        {
            if (ingredient.count == 0U)
                continue;
            for (std::size_t index = 0; index < book.size(); ++index)
                if (!listed[index] && recipe_listed(book[index]) && book[index].result == ingredient.item &&
                    book[index].result_mod_item == ingredient.mod_item)
                {
                    listed[index] = true;
                    list.push_back(index);
                }
        }
    if (list.empty() && !book.empty())
        list.push_back(0U);
    return list;
}

std::vector<std::size_t> Session::type_recipes(const Host &host, RecipeType type) const
{
    const InventoryProfile profile = host.inventory().profile();
    const auto &book = host.recipes();
    std::vector<std::size_t> list;
    for (std::size_t index = 0; index < book.size(); ++index)
        if (recipe_type_for(book[index], profile) == type)
            list.push_back(index);
    return list;
}

std::optional<std::size_t> Session::selected_recipe(const Host &host) const
{
    if (layout() == Layout::classic_crafting)
        return recipe_;
    if (remastered_layout())
        return remastered_recipe(host);
    const RecipeGroup &group = modern_recipe_groups()[group_];
    const auto list = type_recipes(host, group.types[type_[group_]]);
    if (list.empty())
        return std::nullopt;
    return list[std::min(recipe_in_type(), list.size() - 1U)];
}

std::size_t Session::recipe_in_type() const noexcept { return recipe_in_type_[group_][type_[group_]]; }

std::optional<IntRect> Session::strip_rect(int cell) const noexcept
{
    if (layout() != Layout::classic_crafting || cell < 0 || cell > 6)
        return std::nullopt;
    // _itemLocations[l + 2]: the icon's column, nine pixels taller above.
    const int y = classic_background.y + 184 + (cell - 2) * strip_pitch;
    return IntRect{classic_background.x + 39 + 4, truncate(static_cast<float>(y) + 4.0F - 9.0F + scroll_offset_),
                   classic_cell, strip_pitch};
}

std::optional<IntRect> Session::ingredient_rect(const Host &host, int index) const
{
    if (index < 0)
        return std::nullopt;
    if (layout() == Layout::classic_crafting)
    {
        if (!recipe_)
            return std::nullopt;
        const auto &book = host.recipes();
        int count = 0;
        for (const CraftingIngredient &ingredient : book[*recipe_].ingredients)
            count += ingredient.count != 0U ? 1 : 0;
        if (index >= count)
            return std::nullopt;
        return IntRect{classic_background.x + 131 + index * strip_pitch + 4, classic_background.y + 184 + 4,
                       classic_cell, classic_cell};
    }
    if (layout() == Layout::remastered_inventory)
    {
        // The detail card's ingredient lines, stacked upward from the buttons.
        namespace rm = openminerz::remastered;
        const int count = remastered_ingredients(host);
        const auto buttons = craft_rect(false);
        if (index >= count || !buttons)
            return std::nullopt;
        constexpr int line = 24;
        return IntRect{rm::detail_x + 10, buttons->y - 10 - (count - index) * line, rm::detail_width - 20,
                       line - 2};
    }
    if (layout() != Layout::modern_inventory)
        return std::nullopt;
    const Geometry g = geometry();
    const int size = truncate(64.0 * g.s);
    return IntRect{truncate(g.background.x + (39 + 65 * index) * g.s), truncate(g.background.y + 521.0 * g.s), size,
                   size};
}

namespace
{

constexpr std::array<double, 4> group_y{29.0, 77.0, 126.0, 172.0};

IntRect scaled(const IntRect &background, double x, double y, double width, double height, double s) noexcept
{
    return {truncate(background.x + x * s), truncate(background.y + y * s), truncate(width * s), truncate(height * s)};
}

} // namespace

std::optional<IntRect> Session::group_rect(std::size_t group) const noexcept
{
    if (layout() != Layout::modern_inventory || group >= group_y.size())
        return std::nullopt;
    const Geometry g = geometry();
    return scaled(g.background, 14.0, group_y[group], 243.0, 36.0, g.s);
}

std::optional<IntRect> Session::type_rect(std::size_t type) const noexcept
{
    if (layout() != Layout::modern_inventory || type >= modern_recipe_groups()[group_].type_count)
        return std::nullopt;
    const Geometry g = geometry();
    return scaled(g.background, 14.0 + 263.0, group_y[group_] + 7.0 + 31.0 * static_cast<double>(type), 104.0, 30.0,
                  g.s);
}

std::optional<IntRect> Session::recipe_rect(const Host &host, std::size_t index) const
{
    // Remastered: the list row currently showing entry `index` of the tab.
    if (layout() == Layout::remastered_inventory)
        return index >= rm_scroll_ && index < tab_recipes(host).size() ? recipe_row_rect(index - rm_scroll_)
                                                                       : std::nullopt;
    if (layout() != Layout::modern_inventory)
        return std::nullopt;
    const auto list = type_recipes(host, modern_recipe_groups()[group_].types[type_[group_]]);
    if (index >= list.size())
        return std::nullopt;
    const auto &book = host.recipes();
    // Tier2Item: one row per result; another recipe for a result already
    // placed goes to the right of it instead of opening a row.
    std::vector<Point> places(list.size());
    Point next{393, 40};
    for (std::size_t entry = 0; entry < list.size(); ++entry)
    {
        places[entry] = next;
        bool variant = false;
        for (std::size_t earlier = entry; earlier-- > 0U;)
            if (same_result(book[list[entry]], book[list[earlier]]))
            {
                places[entry] = {places[earlier].x + 65, places[earlier].y};
                variant = true;
                break;
            }
        if (!variant)
            next.y += 65;
    }
    const Geometry g = geometry();
    return scaled(g.background, places[index].x, places[index].y, 64.0, 64.0, g.s);
}

// ------------------------------------------------------------------- input

void Session::return_held(Host &host)
{
    if (!held_)
        return;
    // PlayerInventory.AddInventoryItem: what does not fit is thrown at the
    // player's feet.
    const std::uint16_t remainder = host.inventory().add_stack(*held_);
    if (remainder != 0U)
        host.drop(held_->with_count(remainder));
    held_.reset();
}

void Session::open(Host &host, ScreenId screen)
{
    if (screen == ScreenId::none)
    {
        close(host);
        return;
    }
    const bool asked_for_crafting = screen == ScreenId::crafting;
    if (screen == ScreenId::crafting && crafting_in_inventory())
        screen = ScreenId::inventory;
    const ScreenId previous = screen_;
    if (screen == ScreenId::crafting)
        crafting_over_inventory_ = previous == ScreenId::inventory;
    screen_ = screen;
    button_captured_ = false;
    const PlayerInventory &inventory = host.inventory();
    const std::size_t active = tray_base + 8U * inventory.current_tray_index() + inventory.selected_index();
    switch (layout())
    {
    case Layout::classic_inventory:
        // BlockPickerScreen.OnPushed. Coming back from the crafting screen is
        // not a push: the selection stays where it was.
        if (previous != ScreenId::crafting)
            selected_ = active;
        break;
    case Layout::classic_crate:
        selected_ = active;
        break;
    case Layout::classic_crafting:
    {
        // GameScreen.ShowCraftingScreen: DiscoverRecipies, then Reset().
        const auto list = strip_recipes(host);
        recipe_ = list.empty() ? std::nullopt : std::optional<std::size_t>{list.front()};
        ingredient_ = 0;
        scroll_to_ = -1;
        scroll_offset_ = 0.0F;
        break;
    }
    case Layout::modern_inventory:
        // CraftingScreen.OnPushed.
        popup_next_.reset();
        popup_fade_in_ = 0.0F;
        popup_fade_out_ += 0.2F;
        hit_ = false;
        break;
    case Layout::modern_crate:
        // CrateScreen.OnPushed: row 8 is the first tray.
        selected_ = tray_base + inventory.selected_index();
        modern_crate_side_ = false;
        modern_tray_row_ = 8;
        popup_next_.reset();
        popup_fade_in_ = 0.0F;
        popup_fade_out_ += 2.0F;
        hit_ = false;
        break;
    case Layout::remastered_inventory:
    case Layout::remastered_crate:
        // The cursor starts on the item in hand, as in every retail screen.
        // Asking for crafting puts it in the recipe list instead.
        rm_trays_ = static_cast<int>(tray_count(host));
        selected_ = active;
        hit_ = false;
        // Not the pointer's until it moves: the cursor is shown on that slot.
        pointer_active_ = false;
        rm_keys_moved_ = false;
        rm_hot_ = Hot::none;
        rm_hot_index_ = -1;
        rm_recipes_focused_ = asked_for_crafting && screen_ == ScreenId::inventory;
        repeat_wait_ = 0.0F;
        repeat_tick_ = 0.0F;
        if (screen_ == ScreenId::inventory)
            remastered_show_recipe(host);
        break;
    }
    // CrateScreen.OnPushed ends with CrateFocusMessage.Send: the selector is on
    // the tray, so no crate slot is named.
    if (screen_ == ScreenId::crate)
        host.crate_focus(CrateFocusCause::opened, selected_);
}

std::optional<CrateFocus> crate_focus_announcement(CrateFocusCause cause, std::size_t selected,
                                                   bool modern_rules) noexcept
{
    // PopMe(); CrateFocusMessage.Send(gamer, IntVector3.Zero, Point.Zero).
    if (cause == CrateFocusCause::closed)
        return CrateFocus{};
    const bool in_crate = selected >= crate_base && selected < crate_base + crate_slots;
    // 1.6.3.8's mouse branch only moves _selectedLocation. 1.9.9.8's sends
    // when CrateHitTest found the slot, never for an inventory slot.
    if (cause == CrateFocusCause::pointer && (!modern_rules || !in_crate))
        return std::nullopt;
    const int columns = modern_rules ? 4 : 8;
    const int tray_row = modern_rules ? 8 : 4;
    CrateFocus focus;
    focus.in_crate = in_crate;
    if (in_crate || selected < backpack_slots)
    {
        const int index = static_cast<int>(in_crate ? selected - crate_base : selected);
        focus.x = index % columns;
        focus.y = index / columns;
    }
    else
    {
        focus.x = static_cast<int>((selected - tray_base) % 8U);
        focus.y = tray_row;
    }
    return focus;
}

// Screen.PopMe on the crafting screen: back to whatever it was pushed on.
void Session::leave_crafting(Host &host)
{
    button_captured_ = false;
    if (crafting_over_inventory_)
        screen_ = ScreenId::inventory;
    else
        close(host);
}

void Session::close(Host &host)
{
    return_held(host);
    // Both CrateScreens: PopMe(), then CrateFocusMessage.Send(Zero, Point.Zero).
    const bool crate = screen_ == ScreenId::crate;
    screen_ = ScreenId::none;
    button_captured_ = false;
    if (crate)
        host.crate_focus(CrateFocusCause::closed, selected_);
}

void Session::abandon() noexcept
{
    screen_ = ScreenId::none;
    held_.reset();
    button_captured_ = false;
}

void Session::quick_transfer(Host &host)
{
    // (Remastered: not while the cursor is in the recipe list, where there is
    // no slot to send.)
    if (remastered_layout() && rm_recipes_focused_ && !held_)
        return;
    if (is_open() && layout() != Layout::classic_crafting)
        quick_move(host);
}

void Session::place_pointer(int x, int y) noexcept
{
    pointer_ = {x, y};
    if (!is_open())
        return;
    const Point point = to_layout(pointer_);
    const auto close = close_rect();
    close_hover_ = close && close->contains(point);
    const auto button = button_rect();
    button_hover_ = button && button->contains(point);
}

void Session::pointer_moved(Host &host, int x, int y)
{
    if (!is_open())
        return;
    last_pointer_ = pointer_;
    pointer_ = {x, y};
    const Point point = to_layout(pointer_);
    const Layout current = layout();
    // The close box and the frame button follow the pointer every frame.
    const auto close = close_rect();
    close_hover_ = close && close->contains(point);
    const auto button = button_rect();
    button_hover_ = button && button->contains(point);
    if (remastered_layout())
    {
        // A slot under the pointer is the selected slot, as everywhere else;
        // otherwise note which control it rests on.
        pointer_active_ = true;
        rm_trays_ = static_cast<int>(tray_count(host));
        rm_hot_ = Hot::none;
        rm_hot_index_ = -1;
        if (const auto found = slot_at(host, point))
        {
            hit_ = true;
            selected_ = *found;
            rm_recipes_focused_ = false;
            if (screen_ == ScreenId::crate && selected_ >= crate_base && selected_ != crate_pointer_focus_)
            {
                crate_pointer_focus_ = selected_;
                host.crate_focus(CrateFocusCause::pointer, selected_);
            }
        }
        else
        {
            hit_ = false;
            remastered_hover(host, point);
        }
        return;
    }
    if (current == Layout::classic_crafting)
    {
        classic_crafting_hover(host, true);
        hover_done_ = true;
        return;
    }
    pointer_active_ = true;
    if (const auto found = slot_at(host, point))
    {
        hit_ = true;
        selected_ = *found;
        // 1.9.9.8 CrateScreen's mouse branch: an inventory hit clears
        // _selectorInCrateGrid, a crate hit sets it, and Y is the hover index
        // over four (8..11 on the trays).
        modern_crate_side_ = selected_ >= crate_base;
        if (selected_ >= tray_base && selected_ < crate_base)
            modern_tray_row_ = static_cast<int>(selected_ / 4U);
        // 1.9.9.8 CrateScreen: `if (_selectedLocation != _lastSelectedLocation)
        // { _lastSelectedLocation = _selectedLocation; CrateFocusMessage.Send }`
        // inside the CrateHitTest branch only. (1.6.3.8 has no such send; the
        // host decides by the world's rules.)
        if (screen_ == ScreenId::crate && selected_ >= crate_base && selected_ != crate_pointer_focus_)
        {
            crate_pointer_focus_ = selected_;
            host.crate_focus(CrateFocusCause::pointer, selected_);
        }
    }
    else
    {
        hit_ = false;
    }
    if (recipe_tree_shown())
    {
        modern_crafting_hover(host, false, false);
        hover_done_ = true;
    }
}

// InventoryItem.Split / PopOneItem create new items; Stack moves counts.
void Session::pick_or_place(Host &host)
{
    PlayerInventory::Slot *target = slot(host, selected_);
    if (target == nullptr)
        return;
    if (!held_)
    {
        if (target->has_value())
        {
            held_ = *target;
            write(host, selected_, std::nullopt);
        }
        host.play(Cue::click);
        return;
    }
    if (target->has_value() && (*target)->same_kind(*held_) &&
        (*target)->count < limit_of(host, held_->item))
    {
        InventoryStack merged = **target;
        stack_into(host, merged, *held_);
        write(host, selected_, merged);
        if (held_->count == 0U)
            held_.reset();
    }
    else
    {
        PlayerInventory::Slot previous = *target;
        write(host, selected_, held_);
        held_ = previous;
    }
    host.play(Cue::click);
}

void Session::split_or_place_one(Host &host)
{
    PlayerInventory::Slot *target = slot(host, selected_);
    if (target == nullptr)
        return;
    if (!held_)
    {
        if (!target->has_value())
            return;
        host.play(Cue::click);
        if ((*target)->count == 1U)
        {
            held_ = *target;
            write(host, selected_, std::nullopt);
        }
        else
        {
            const auto half = static_cast<std::uint16_t>((*target)->count / 2U);
            held_ = fresh(**target, half);
            write(host, selected_, (*target)->with_count(static_cast<std::uint16_t>((*target)->count - half)));
        }
        return;
    }
    host.play(Cue::click);
    if (target->has_value())
    {
        if (!(*target)->same_kind(*held_))
        {
            PlayerInventory::Slot previous = *target;
            write(host, selected_, held_);
            held_ = previous;
            return;
        }
        if ((*target)->count >= limit_of(host, held_->item))
            return;
        write(host, selected_, (*target)->with_count(static_cast<std::uint16_t>((*target)->count + 1U)));
        if (held_->count > 1U)
            held_->count = static_cast<std::uint16_t>(held_->count - 1U);
        else
            held_.reset();
        return;
    }
    if (held_->count > 1U)
    {
        write(host, selected_, fresh(*held_, 1U));
        held_->count = static_cast<std::uint16_t>(held_->count - 1U);
    }
    else
    {
        write(host, selected_, held_);
        held_.reset();
    }
}

void Session::quick_move(Host &host)
{
    PlayerInventory &inventory = host.inventory();
    if (screen_ != ScreenId::crate)
    {
        // BlockPickerScreen / CraftingScreen: the held stack, or else the
        // selected one, goes to the other section (backpack <-> tray).
        const bool to_tray = selected_ < backpack_slots;
        if (held_)
        {
            const std::uint16_t remainder = inventory.add_stack_to_section(*held_, to_tray);
            if (remainder == 0U)
                held_.reset();
            else if (remainder != held_->count)
                held_->count = remainder;
            else
                return;
            host.play(Cue::click);
            return;
        }
        PlayerInventory::Slot *source = slot(host, selected_);
        if (source == nullptr || !source->has_value())
            return;
        const InventoryStack moving = **source;
        const std::uint16_t remainder = inventory.add_stack_to_section(moving, to_tray);
        if (remainder == moving.count)
            return;
        write(host, selected_, remainder == 0U ? PlayerInventory::Slot{} : PlayerInventory::Slot{moving.with_count(remainder)});
        host.play(Cue::click);
        return;
    }
    // CrateScreen.SwapHoldingItemLocation / SwapSelectedItemLocation: across
    // to the other container. The click only sounds when the whole stack went.
    const bool to_crate = selected_ < crate_base;
    const auto into_crate = [&](InventoryStack &moving) {
        for (std::size_t index = 0; index < crate_slots && moving.count != 0U; ++index)
        {
            PlayerInventory::Slot *target = host.crate_slot(index);
            if (target == nullptr || !target->has_value() || host.crate_slot_locked(index))
                continue;
            const std::uint16_t before = moving.count;
            InventoryStack merged = **target;
            stack_into(host, merged, moving);
            if (moving.count != before)
                write(host, crate_base + index, merged);
        }
        if (moving.count == 0U)
            return;
        for (std::size_t index = 0; index < crate_slots; ++index)
        {
            PlayerInventory::Slot *target = host.crate_slot(index);
            if (target == nullptr || target->has_value() || host.crate_slot_locked(index))
                continue;
            write(host, crate_base + index, moving);
            moving.count = 0U;
            return;
        }
    };
    if (held_)
    {
        if (to_crate)
            into_crate(*held_);
        else
            held_->count = inventory.add_stack(*held_);
        if (held_->count == 0U)
        {
            held_.reset();
            host.play(Cue::click);
        }
        return;
    }
    PlayerInventory::Slot *source = slot(host, selected_);
    if (source == nullptr || !source->has_value() || selected_locked(host))
        return;
    InventoryStack moving = **source;
    if (to_crate)
        into_crate(moving);
    else
        moving.count = inventory.add_stack(moving);
    if (moving.count == (*source)->count)
        return;
    write(host, selected_, moving.count == 0U ? PlayerInventory::Slot{} : PlayerInventory::Slot{moving});
    if (moving.count == 0U)
        host.play(Cue::click);
}

void Session::drop_selected(Host &host)
{
    if (screen_ == ScreenId::crate && selected_locked(host))
    {
        host.play(Cue::error);
        return;
    }
    host.play(Cue::click);
    if (held_)
    {
        host.drop(*held_);
        host.play(Cue::drop_item);
        held_.reset();
        return;
    }
    PlayerInventory::Slot *target = slot(host, selected_);
    if (target == nullptr || !target->has_value())
        return;
    const InventoryStack dropped = **target;
    write(host, selected_, std::nullopt);
    host.drop(dropped);
    host.play(Cue::drop_item);
}

bool Session::count_down(Host &host)
{
    if (screen_ == ScreenId::crate && selected_locked(host))
        return false;
    PlayerInventory::Slot *target = slot(host, selected_);
    if (!held_ || target == nullptr)
        return false;
    if (!target->has_value())
    {
        if (held_->count == 1U)
        {
            write(host, selected_, held_);
            held_.reset();
        }
        else
        {
            write(host, selected_, fresh(*held_, 1U));
            held_->count = static_cast<std::uint16_t>(held_->count - 1U);
        }
        return true;
    }
    if (!(*target)->same_kind(*held_) || (*target)->count >= limit_of(host, held_->item))
        return false;
    write(host, selected_, (*target)->with_count(static_cast<std::uint16_t>((*target)->count + 1U)));
    if (held_->count == 1U)
        held_.reset();
    else
        held_->count = static_cast<std::uint16_t>(held_->count - 1U);
    return true;
}

bool Session::count_up(Host &host)
{
    if (screen_ == ScreenId::crate && selected_locked(host))
        return false;
    PlayerInventory::Slot *target = slot(host, selected_);
    if (target == nullptr || !target->has_value())
        return false;
    if (!held_)
    {
        if ((*target)->count == 1U)
        {
            held_ = *target;
            write(host, selected_, std::nullopt);
        }
        else
        {
            held_ = fresh(**target, 1U);
            write(host, selected_, (*target)->with_count(static_cast<std::uint16_t>((*target)->count - 1U)));
        }
        return true;
    }
    if (!(*target)->same_kind(*held_) || held_->count >= limit_of(host, held_->item))
        return false;
    held_->count = static_cast<std::uint16_t>(held_->count + 1U);
    if ((*target)->count == 1U)
        write(host, selected_, std::nullopt);
    else
        write(host, selected_, (*target)->with_count(static_cast<std::uint16_t>((*target)->count - 1U)));
    return true;
}

void Session::move_selection(Host &host, int horizontal, int vertical)
{
    const Layout current = layout();
    const std::size_t visible = host.inventory().current_tray_index();
    if (current == Layout::classic_inventory || current == Layout::classic_crate)
    {
        // _selectedLocation: rows 0..3 a grid, row 4 the tray.
        const bool crate = selected_ >= crate_base;
        const std::size_t local = crate ? selected_ - crate_base : selected_;
        int column = static_cast<int>(local < backpack_slots ? local % 8U : (local - tray_base) % 8U);
        int row = local < backpack_slots ? static_cast<int>(local / 8U) : 4;
        bool in_crate = crate;
        column = (column + horizontal + 8) % 8;
        row += vertical;
        if (current == Layout::classic_inventory)
        {
            row = (row + 5) % 5;
        }
        else if (in_crate)
        {
            // crate rows -> backpack rows -> tray -> crate
            if (row > 3)
            {
                row = 0;
                in_crate = false;
            }
            else if (row < 0)
            {
                row = 4;
                in_crate = false;
            }
        }
        else if (row > 4)
        {
            row = 0;
            in_crate = true;
        }
        else if (row < 0)
        {
            row = 3;
            in_crate = true;
        }
        const auto cell = static_cast<std::size_t>(column);
        if (in_crate)
            selected_ = crate_base + static_cast<std::size_t>(row) * 8U + cell;
        else if (row == 4)
            selected_ = tray_base + 8U * visible + cell;
        else
            selected_ = static_cast<std::size_t>(row) * 8U + cell;
        return;
    }
    if (current != Layout::modern_crate)
        return;
    // 1.9.9.8 CrateScreen: _selectedLocation is X 0..3 by Y 0..8, rows 0..7 one
    // of the two grids and row 8 the tray. Which grid is _setSelectorToCrate,
    // which the selector keeps while it is on the tray row:
    //   SelectDown  Y++ (past 8 -> 0)        SelectUp     Y-- (below 0 -> 8)
    //   SelectLeft  X--, below 0:  flip the grid, X = 3
    //   SelectRight X++, above 3:  flip the grid, X = 0
    // "Flip" is `_selectorInCrateGrid = !_selectorInCrateGrid`, whose getter is
    // false on the tray row, so stepping off a side THERE always chooses the
    // crate. (Executed: ScreensParityProbe script crate.focus_keys.)
    const bool on_tray = selected_ >= tray_base && selected_ < crate_base;
    const std::size_t local = selected_ >= crate_base ? selected_ - crate_base : selected_;
    int x = static_cast<int>(on_tray ? (local - tray_base) % 8U : local % 4U);
    int y = on_tray ? modern_tray_row_ : static_cast<int>(local / 4U);
    bool to_crate = on_tray ? modern_crate_side_ : selected_ >= crate_base;
    if (vertical > 0)
        y = y + 1 > 8 ? 0 : y + 1;
    else if (vertical < 0)
        y = y - 1 < 0 ? 8 : y - 1;
    const bool in_crate_grid = to_crate && y < 8;
    if (horizontal < 0 && --x < 0)
    {
        to_crate = !in_crate_grid;
        x = 3;
    }
    else if (horizontal > 0 && ++x > 3)
    {
        to_crate = !in_crate_grid;
        x = 0;
    }
    modern_crate_side_ = to_crate;
    if (y >= 8)
    {
        // GetTrayIndexFromRow: (Y - 8) / 2.
        modern_tray_row_ = y;
        selected_ = tray_base + static_cast<std::size_t>((y - 8) / 2) * 8U + static_cast<std::size_t>(std::clamp(x, 0, 7));
    }
    else
    {
        // (A tray column past 3 carried up into a grid names no slot in the
        // original; the nearest one is kept here.)
        selected_ = (to_crate ? crate_base : 0U) + static_cast<std::size_t>(y) * 4U +
                    static_cast<std::size_t>(std::clamp(x, 0, 3));
    }
}

void Session::button_pressed(Host &host, Button button, bool shift)
{
    if (!is_open())
        return;
    if (remastered_layout())
    {
        remastered_button(host, button, shift);
        return;
    }
    const Point point = to_layout(pointer_);
    const Layout current = layout();
    const auto frame_button = button_rect();
    const bool on_button = frame_button && frame_button->contains(point);
    if (current == Layout::classic_crafting)
    {
        if (button != Button::left)
            return;
        // CraftingUIScreen: a click on the inventory grid or the close box
        // returns to the inventory screen, silently.
        if (classic_crafting_grid.contains(point) || close_hover_)
        {
            leave_crafting(host);
            return;
        }
        classic_crafting_button(host, shift, false);
        if (on_button && recipe_ &&
            host.can_craft(host.recipes()[*recipe_]))
            button_captured_ = true;
        return;
    }
    const bool crate = screen_ == ScreenId::crate;
    // The crate screens test _hitTestTrue; the others hit-test the pointer.
    const bool on_slot = crate ? hit_ : slot_at(host, point).has_value();
    const bool outside = !geometry().background.contains(point);
    if (button == Button::left)
    {
        // Tier2Item.CheckInput runs before the slot branches.
        if (recipe_tree_shown() && !held_)
            modern_crafting_hover(host, true, shift);
        if (shift && on_slot)
        {
            quick_move(host);
        }
        else if (on_slot)
        {
            if (crate && selected_locked(host))
                host.play(Cue::error);
            else
                pick_or_place(host);
        }
        else if (current == Layout::classic_inventory && close_hover_)
        {
            host.play(Cue::click);
            close(host);
            return;
        }
        else if (outside && held_)
        {
            throw_held(host, false);
        }
        if (on_button)
            button_captured_ = true;
        return;
    }
    if (on_slot)
    {
        if (crate && selected_locked(host))
            host.play(Cue::error);
        else
            split_or_place_one(host);
    }
    else if (outside && held_)
    {
        throw_held(host, true);
    }
}

void Session::throw_held(Host &host, bool one)
{
    if (!held_)
        return;
    // Dropped at the player's feet as a pickup.
    pointer_active_ = true;
    host.play(Cue::click);
    if (one && held_->count > 1U)
    {
        host.drop(fresh(*held_, 1U));
        held_->count = static_cast<std::uint16_t>(held_->count - 1U);
    }
    else
    {
        host.drop(*held_);
        held_.reset();
    }
    host.play(Cue::drop_item);
}

void Session::button_released(Host &host, Button button)
{
    if (!is_open() || button != Button::left)
        return;
    const bool pressed = button_captured_;
    button_captured_ = false;
    const auto frame_button = button_rect();
    if (!pressed || !frame_button || !frame_button->contains(to_layout(pointer_)))
        return;
    if (layout() == Layout::classic_crafting)
    {
        // _craftButton_Pressed: one craft; Shift is the icon's shortcut only.
        const auto &book = host.recipes();
        if (!recipe_ || !host.can_craft(book[*recipe_]))
        {
            host.play(Cue::error);
            return;
        }
        host.craft(*recipe_, false);
        ingredient_ = 0;
        host.play(Cue::craft);
        return;
    }
    // BlockPickerScreen._craftButton_Pressed.
    return_held(host);
    host.play(Cue::click);
    open(host, ScreenId::crafting);
}

void Session::wheel(Host &host, int direction)
{
    if (!is_open() || direction == 0)
        return;
    if (remastered_layout())
    {
        const Point point = to_layout(pointer_);
        // Over the recipe list the wheel scrolls it. Over a slot it is the
        // original's wheel: one item to or from the cursor.
        namespace rm = openminerz::remastered;
        const IntRect list{rm::left_x, rm::recipe_list_y, rm::recipe_list_width,
                           rm::recipe_rows(rm_trays_) * rm::recipe_row_height};
        if (screen_ == ScreenId::inventory && list.contains(point))
        {
            const auto total = tab_recipes(host).size();
            const auto rows = static_cast<std::size_t>(rm::recipe_rows(rm_trays_));
            const std::size_t last = total > rows ? total - rows : 0U;
            if (direction < 0)
                rm_scroll_ = std::min(last, rm_scroll_ + 2U);
            else
                rm_scroll_ = rm_scroll_ > 2U ? rm_scroll_ - 2U : 0U;
            remastered_hover(host, point);
            return;
        }
        if (!slot_at(host, point).has_value())
            return;
        pointer_active_ = true;
        if (direction < 0 ? count_down(host) : count_up(host))
            host.play(Cue::click);
        return;
    }
    const Layout current = layout();
    if (current == Layout::classic_crafting)
    {
        const auto list = strip_recipes(host);
        const int position = strip_position(host);
        const int next = position + (direction < 0 ? 1 : -1);
        if (next >= 0 && next < static_cast<int>(list.size()) && position >= 0)
        {
            recipe_ = list[static_cast<std::size_t>(next)];
            ingredient_ = 0;
            host.play(Cue::click);
        }
        return;
    }
    const bool crate = screen_ == ScreenId::crate;
    const bool on_slot = crate ? hit_ : slot_at(host, to_layout(pointer_)).has_value();
    if (!on_slot)
        return;
    pointer_active_ = true;
    if (direction < 0 ? count_down(host) : count_up(host))
        host.play(Cue::click);
}

void Session::key_pressed(Host &host, Key key, bool shift)
{
    if (!is_open())
        return;
    if (remastered_layout())
    {
        remastered_key(host, key, shift);
        return;
    }
    const Layout current = layout();
    const bool arrow = key == Key::up || key == Key::down || key == Key::left || key == Key::right;
    if (current == Layout::classic_crafting)
    {
        const auto list = strip_recipes(host);
        const int position = strip_position(host);
        const auto &book = host.recipes();
        switch (key)
        {
        case Key::escape:
        case Key::inventory:
            host.play(Cue::click);
            leave_crafting(host);
            break;
        case Key::enter:
            classic_crafting_button(host, shift, true);
            break;
        case Key::down:
        case Key::up:
        {
            const int next = position + (key == Key::down ? 1 : -1);
            if (position >= 0 && next >= 0 && next < static_cast<int>(list.size()))
            {
                recipe_ = list[static_cast<std::size_t>(next)];
                ingredient_ = 0;
                host.play(Cue::click);
            }
            break;
        }
        case Key::left:
            if (ingredient_ > 0)
            {
                --ingredient_;
                host.play(Cue::click);
            }
            break;
        case Key::right:
        {
            int count = 0;
            if (recipe_)
                for (const CraftingIngredient &ingredient : book[*recipe_].ingredients)
                    count += ingredient.count != 0U ? 1 : 0;
            if (recipe_ && ingredient_ < count)
            {
                ++ingredient_;
                host.play(Cue::click);
            }
            break;
        }
        default:
            break;
        }
        return;
    }
    const bool crate = screen_ == ScreenId::crate;
    const bool keyboard_grid = current != Layout::modern_inventory;
    if (arrow)
    {
        if (!keyboard_grid)
            return; // 1.9.9.8's CraftingScreen is pointer only
        pointer_active_ = false;
        repeat_wait_ = 0.0F;
        repeat_tick_ = 0.0F;
        // 1.9.9.8's CrateScreen reads the vertical keys crossed on the press as
        // well as on the repeat: `WasKeyPressed(Keys.Up)` is in the SelectDown
        // test and Keys.Down in SelectUp's. 1.6.3.8 crosses only the repeat.
        const int press_down = current == Layout::modern_crate ? -1 : 1;
        move_selection(host, key == Key::left ? -1 : key == Key::right ? 1 : 0,
                       key == Key::up ? -press_down : key == Key::down ? press_down : 0);
        host.play(Cue::click);
        // CrateScreen sends CrateFocusMessage after every arrow PRESS, with the
        // crate's location only while the selector is in the crate grid. The
        // held-key repeat in update() moves the selector without sending.
        if (crate)
            host.crate_focus(CrateFocusCause::key, selected_);
        return;
    }
    switch (key)
    {
    case Key::enter:
        if (!keyboard_grid)
            break;
        if (crate && selected_locked(host))
            host.play(Cue::error);
        else
            pick_or_place(host);
        break;
    case Key::drop:
        drop_selected(host);
        break;
    case Key::escape:
        if (current == Layout::modern_inventory && held_)
        {
            // CraftingScreen: the first Escape only puts the stack back.
            return_held(host);
            host.play(Cue::click);
            break;
        }
        host.play(Cue::click);
        // (1.6.3.8's CrateScreen forgets the held stack here; this hands it
        // back, as 1.9.9.8's does.)
        return_held(host);
        if (screen_ == ScreenId::crafting)
            leave_crafting(host);
        else
            close(host);
        break;
    case Key::inventory:
        if (crate)
            break; // neither CrateScreen reads E
        return_held(host);
        host.play(Cue::click);
        if (current == Layout::classic_inventory)
            open(host, ScreenId::crafting); // GameScreen.ShowCraftingScreen
        else if (screen_ == ScreenId::crafting)
            leave_crafting(host);
        else
            close(host);
        break;
    default:
        break;
    }
}

int Session::strip_position(const Host &host) const
{
    if (!recipe_)
        return -1;
    const auto list = strip_recipes(host);
    const auto found = std::find(list.begin(), list.end(), *recipe_);
    return found == list.end() ? -1 : static_cast<int>(found - list.begin());
}

void Session::classic_crafting_hover(Host &host, bool moved)
{
    const Point point = to_layout(pointer_);
    const auto list = strip_recipes(host);
    const int selected = strip_position(host);
    int ingredient = -1;
    for (int index = 0; index < 4; ++index)
        if (const auto rect = ingredient_rect(host, index); rect && rect->contains(point))
            ingredient = index;
    if (ingredient >= 0)
    {
        draw_selector_ = selected;
        if (ingredient_ != ingredient + 1)
        {
            host.play(Cue::click);
            ingredient_ = ingredient + 1;
        }
    }
    if (scroll_to_ != -1 && !moved)
    {
        moved_during_scroll_ = false;
        return;
    }
    moved_during_scroll_ = true;
    if (ingredient >= 0)
        return;
    const auto exists = [&](int cell) {
        const int index = selected + cell - 2;
        return index >= 0 && index < static_cast<int>(list.size());
    };
    int other = -1;
    for (int cell = 0; cell < 7; ++cell)
        if (cell != 2 && exists(cell))
            if (const auto rect = strip_rect(cell); rect && rect->contains(point))
            {
                other = cell;
                break;
            }
    const auto own = strip_rect(2);
    if (ingredient_ != 0 && exists(2) && own && own->contains(point))
    {
        draw_selector_ = selected;
        host.play(Cue::click);
        ingredient_ = 0;
    }
    else if (other >= 0)
    {
        if (draw_selector_ != other - 2 + selected)
        {
            host.play(Cue::click);
            ingredient_ = 0;
            draw_selector_ = other - 2 + selected;
        }
    }
    else if (draw_selector_ != selected)
    {
        host.play(Cue::click);
        draw_selector_ = selected;
    }
}

void Session::classic_crafting_button(Host &host, bool shift, bool)
{
    const Point point = to_layout(pointer_);
    const auto list = strip_recipes(host);
    const int selected = strip_position(host);
    const auto &book = host.recipes();
    int hovered_ingredient = -1;
    for (int index = 0; index < 4; ++index)
        if (const auto rect = ingredient_rect(host, index); rect && rect->contains(point))
            hovered_ingredient = index;
    if (ingredient_ > 0)
    {
        if (!recipe_ || hovered_ingredient < 0)
        {
            host.play(Cue::error);
            return;
        }
        // Jump to the recipe that makes the selected component.
        CraftingIngredient wanted{};
        int seen = 0;
        for (const CraftingIngredient &ingredient : book[*recipe_].ingredients)
            if (ingredient.count != 0U && ++seen == ingredient_)
                wanted = ingredient;
        for (std::size_t index = 0; index < list.size(); ++index)
            if (book[list[index]].result == wanted.item && book[list[index]].result_mod_item == wanted.mod_item)
            {
                scroll_to_ = static_cast<int>(index);
                ingredient_ = 0;
                host.play(Cue::click);
                return;
            }
        host.play(Cue::error);
        return;
    }
    const auto own = strip_rect(2);
    if (selected >= 0 && own && own->contains(point))
    {
        if (!recipe_ || !host.can_craft(book[*recipe_]))
        {
            host.play(Cue::error);
            return;
        }
        host.craft(*recipe_, craft_every(host, shift));
        ingredient_ = 0;
        host.play(Cue::craft);
        return;
    }
    for (int cell = 0; cell < 7; ++cell)
    {
        const int index = selected + cell - 2;
        if (cell == 2 || index < 0 || index >= static_cast<int>(list.size()))
            continue;
        if (const auto rect = strip_rect(cell); rect && rect->contains(point))
        {
            // Scroll the strip to it; a second click then crafts.
            scroll_to_ = index;
            ingredient_ = 0;
            return;
        }
    }
}

void Session::modern_crafting_hover(Host &host, bool pressed, bool shift)
{
    if (held_ || !recipe_tree_shown())
        return;
    const Point point = pointer_;
    const auto &groups = modern_recipe_groups();
    for (std::size_t group = 0; group < groups.size(); ++group)
    {
        if (group == group_)
        {
            for (std::size_t type = 0; type < groups[group].type_count; ++type)
            {
                if (type == type_[group])
                {
                    const auto list = type_recipes(host, groups[group].types[type]);
                    for (std::size_t index = 0; index < list.size(); ++index)
                    {
                        const auto rect = recipe_rect(host, index);
                        if (!rect || !rect->contains(point))
                            continue;
                        recipe_in_type_[group][type] = index;
                        if (pressed)
                        {
                            const auto &book = host.recipes();
                            if (host.can_craft(book[list[index]]))
                            {
                                host.play(Cue::craft);
                                host.craft(list[index], craft_every(host, shift));
                            }
                            else
                            {
                                host.play(Cue::error);
                            }
                        }
                        break;
                    }
                }
                if (const auto rect = type_rect(type); rect && rect->contains(point))
                    type_[group] = type;
            }
        }
        if (const auto rect = group_rect(group); rect && rect->contains(point))
            group_ = group;
    }
}

void Session::update_popup(const Host &host, float seconds)
{
    if (!modern_layout())
        return;
    const float wait = layout() == Layout::modern_crate ? 1.0F : 0.25F;
    const PlayerInventory::Slot *target = slot(host, selected_);
    if (!held_ && target != nullptr && target->has_value() && hit_)
    {
        if (popup_next_ == selected_)
        {
            if (popup_wait_ >= wait)
            {
                if (popup_fade_in_ < 0.25F)
                {
                    popup_fade_in_ += seconds;
                    popup_fade_out_length_ = popup_fade_in_;
                    popup_fade_out_ = 0.0F;
                }
            }
            else
            {
                popup_fade_out_ += seconds;
                popup_wait_ += seconds;
                if (popup_wait_ >= wait)
                {
                    popup_item_ = **target;
                    popup_location_ = pointer_;
                }
            }
        }
        else
        {
            popup_next_ = selected_;
            popup_wait_ = 0.0F;
            popup_fade_in_ = 0.0F;
            popup_fade_out_ += seconds;
        }
    }
    else
    {
        popup_fade_out_ += seconds;
        popup_next_.reset();
        popup_fade_in_ = 0.0F;
    }
}

void Session::update(Host &host, float seconds, HeldKeys held)
{
    if (!is_open())
        return;
    if (remastered_layout())
    {
        rm_trays_ = static_cast<int>(tray_count(host));
        if (screen_ == ScreenId::inventory)
        {
            // The list changes under the cursor as items come and go (a
            // Classic world lists what has been discovered): keep the
            // selection on a listed recipe and the scroll inside the list.
            const auto list = tab_recipes(host);
            const auto rows = static_cast<std::size_t>(openminerz::remastered::recipe_rows(rm_trays_));
            rm_scroll_ = std::min(rm_scroll_, list.size() > rows ? list.size() - rows : 0U);
            if (!rm_recipe_ || std::find(list.begin(), list.end(), *rm_recipe_) == list.end())
                rm_recipe_ = list.empty() ? std::nullopt : std::optional<std::size_t>{list.front()};
        }
        // A held arrow repeats in the direction it points (the retail screens'
        // crossed Up/Down repeat is theirs alone).
        const int horizontal = held.left ? -1 : held.right ? 1 : 0;
        const int vertical = held.up ? -1 : held.down ? 1 : 0;
        if (horizontal == 0 && vertical == 0)
        {
            repeat_wait_ = 0.0F;
            repeat_tick_ = 0.0F;
            return;
        }
        repeat_wait_ += seconds;
        if (repeat_wait_ < 0.4F)
            return;
        repeat_tick_ += seconds;
        if (repeat_tick_ < 0.08F)
            return;
        repeat_tick_ = 0.0F;
        remastered_move(host, vertical != 0 ? 0 : horizontal, vertical);
        return;
    }
    const Layout current = layout();
    if (current == Layout::classic_crafting)
    {
        if (!hover_done_)
            classic_crafting_hover(host, false);
        hover_done_ = false;
        // CraftingUIScreen.Update: the strip slides 462 px/s toward the
        // recipe that was clicked, one 77 px cell at a time.
        if (scroll_to_ >= 0)
        {
            const auto list = strip_recipes(host);
            const int position = strip_position(host);
            const auto select = [&](int index) {
                recipe_ = index >= 0 && index < static_cast<int>(list.size())
                              ? std::optional<std::size_t>{list[static_cast<std::size_t>(index)]}
                              : std::nullopt;
                ingredient_ = 0;
            };
            if (scroll_to_ < position)
            {
                scroll_offset_ += 462.0F * seconds;
                if (scroll_offset_ >= 77.0F)
                {
                    scroll_offset_ = 0.0F;
                    select(position - 1);
                    if (!moved_during_scroll_)
                        draw_selector_ = draw_selector_ > 0 ? draw_selector_ - 1 : position - 1;
                }
            }
            else if (scroll_to_ > position)
            {
                scroll_offset_ -= 462.0F * seconds;
                if (scroll_offset_ <= -77.0F)
                {
                    scroll_offset_ = 0.0F;
                    select(position + 1);
                    if (!moved_during_scroll_)
                        draw_selector_ = draw_selector_ < static_cast<int>(list.size()) - 1 ? draw_selector_ + 1
                                                                                             : position + 1;
                }
            }
            else
            {
                scroll_to_ = -1;
                scroll_offset_ = 0.0F;
            }
        }
        return;
    }
    if (recipe_tree_shown() && !hover_done_)
        modern_crafting_hover(host, false, false);
    hover_done_ = false;
    update_popup(host, seconds);
    if (current == Layout::modern_inventory)
        return;
    // waitScrollTimer / autoScrollTimer. The original's held-key branch is
    // crossed over: holding Up repeats SelectDown and holding Down SelectUp.
    repeat_wait_ += seconds;
    if (repeat_wait_ < 0.5F)
        return;
    int horizontal = 0;
    int vertical = 0;
    if (held.up)
        vertical = 1;
    else if (held.down)
        vertical = -1;
    else if (held.left)
        horizontal = -1;
    else if (held.right)
        horizontal = 1;
    else
        return;
    repeat_tick_ += seconds;
    if (repeat_tick_ < 0.1F)
        return;
    repeat_tick_ = 0.0F;
    move_selection(host, horizontal, vertical);
    host.play(Cue::click);
}

// -------------------------------------------------------------------- draw

namespace
{

struct Painter final
{
    DrawList &out;
    const Host &host;
    const TextMetrics &metrics;
    bool modern{};
    float s{1.0F};

    // InventoryItemClass.Draw2D.
    void icon(const InventoryStack &stack, IntRect dest, Color color) const
    {
        const bool large = modern && static_cast<float>(dest.width) / 64.0F > 1.1F;
        Draw draw = sprite(large ? Sprite::item_icon_large : Sprite::item_icon, dest, color);
        draw.icon = retail_icon(stack.item, modern ? UiStyle::modern : UiStyle::classic);
        draw.item = stack.item;
        draw.mod_item = stack.mod_item; // Modding, visuals area
        out.push_back(std::move(draw));
    }

    // InventoryItem.Draw2D(spriteBatch, dest, color[, drawAmt]).
    void item(const InventoryStack &stack, IntRect dest, Color color, bool amount = true) const
    {
        icon(stack, dest, color);
        if (stack.count > 1U && amount)
            out.push_back(text(Font::small, std::to_string(stack.count), static_cast<float>(dest.x + 8),
                               static_cast<float>(dest.y + dest.height - metrics.line_spacing(Font::small)), white, 1,
                               modern ? s : 1.0F));
    }

    // InventoryItem.Draw2D(spriteBatch, dest): the wear bar goes UNDER the icon.
    void slot(const InventoryStack &stack, IntRect dest) const
    {
        if (item_self_damage_per_use(stack.item, host.inventory().profile()) > 0.0F)
        {
            out.push_back(sprite(Sprite::solid, {dest.x + 9, dest.bottom() - 16, dest.width - 18, 7}, black));
            out.push_back(sprite(Sprite::solid,
                                 {dest.x + 10, dest.bottom() - 15,
                                  truncate((dest.width - 20) * static_cast<double>(stack.durability)), 5},
                                 wear_green));
        }
        item(stack, dest, white);
    }

    // ScalableFrame.Draw and the centred caption (FrameButtonControl.OnDraw).
    void button(IntRect bounds, std::string_view caption, bool enabled, bool captured, bool hovering,
                float scale) const
    {
        Color frame = white;
        Color ink = black;
        if (!enabled)
        {
            frame = gray;
            ink = dim_gray;
        }
        else if (captured)
        {
            frame = black;
            ink = white;
        }
        else if (hovering)
        {
            frame = gray;
        }
        const int right = bounds.right();
        const int bottom = bounds.bottom();
        out.push_back(sprite(Sprite::frame_top_left, {bounds.x, bounds.y, 2, 2}, frame));
        out.push_back(sprite(Sprite::frame_top_right, {right - 2, bounds.y, 2, 2}, frame));
        out.push_back(sprite(Sprite::frame_bottom_left, {bounds.x, bottom - 2, 2, 2}, frame));
        out.push_back(sprite(Sprite::frame_bottom_right, {right - 2, bottom - 2, 2, 2}, frame));
        out.push_back(sprite(Sprite::frame_top, {bounds.x + 2, bounds.y, bounds.width - 4, 2}, frame));
        out.push_back(sprite(Sprite::frame_bottom, {bounds.x + 2, bottom - 2, bounds.width - 4, 2}, frame));
        out.push_back(sprite(Sprite::frame_left, {bounds.x, bounds.y + 2, 2, bounds.height - 4}, frame));
        out.push_back(sprite(Sprite::frame_right, {right - 2, bounds.y + 2, 2, bounds.height - 4}, frame));
        out.push_back(sprite(Sprite::frame_center, {bounds.x + 2, bounds.y + 2, bounds.width - 4, bounds.height - 4}, frame));
        const Extent size = metrics.measure(Font::medium, caption);
        const Point center = bounds.center();
        out.push_back(text(Font::medium, std::string{caption}, static_cast<float>(center.x) - size.width * scale / 2.0F,
                           static_cast<float>(center.y) -
                               static_cast<float>(metrics.line_spacing(Font::medium)) * scale / 2.0F,
                           ink, 0, scale));
    }

    // Name, then Description1 and Description2 one under the other.
    void classic_info(const InventoryStack &stack, int top) const
    {
        const ItemText words = host.item_words(stack, InventoryProfile::classic);
        float y = static_cast<float>(top);
        out.push_back(text(Font::medium, std::string{words.name}, 404.0F, y, white, 2));
        y += metrics.measure(Font::medium, words.name).height;
        out.push_back(text(Font::small, std::string{words.description1}, 404.0F, y, white, 1));
        y += metrics.measure(Font::small, words.description1).height;
        out.push_back(text(Font::small, std::string{words.description2}, 404.0F, y, white, 1));
    }

    void close_box(bool hover) const
    {
        const IntRect box{classic_background.right() - 43, classic_background.y + 15, 25, 25};
        out.push_back(sprite(Sprite::solid, {box.x - 1, box.y - 1, box.width + 2, box.height + 2}, black));
        out.push_back(sprite(Sprite::solid, box, hover ? light_gray : gray));
        out.push_back(text(Font::myriad_medium, "x", static_cast<float>(box.x + 5), static_cast<float>(box.y - 5),
                           hover ? white : dark_gray, 1));
    }
};

} // namespace

DrawList Session::draw(const Host &host, const TextMetrics &metrics) const
{
    DrawList out;
    if (!is_open())
        return out;
    switch (layout())
    {
    case Layout::classic_inventory:
        draw_classic_inventory(host, metrics, out);
        break;
    case Layout::classic_crafting:
        draw_classic_crafting(host, metrics, out);
        break;
    case Layout::classic_crate:
        draw_classic_crate(host, metrics, out);
        break;
    case Layout::modern_inventory:
        draw_modern_inventory(host, metrics, out);
        break;
    case Layout::modern_crate:
        draw_modern_crate(host, metrics, out);
        break;
    case Layout::remastered_inventory:
    case Layout::remastered_crate:
        // Remastered is not a recording of anything: the application draws it
        // directly from this session's rectangles and state.
        break;
    }
    return out;
}

void Session::draw_classic_inventory(const Host &host, const TextMetrics &metrics, DrawList &out) const
{
    Geometry g = geometry();
    g.visible_tray = host.inventory().current_tray_index();
    g.tray_count = tray_count(host);
    const Painter paint{out, host, metrics, false, 1.0F};
    out.push_back(sprite(Sprite::block_ui_back, g.background));
    paint.close_box(close_hover_);
    // Strings.Right_Click + " : " + Strings.Split_stack, one MedFont line apart.
    const float line = metrics.measure(Font::medium, phrase(RetailString::Split_Items, false) + " ").height;
    out.push_back(text(Font::medium,
                       phrase(RetailString::Right_Click, false) + " : " + phrase(RetailString::Split_stack, false), 404.0F,
                       180.0F + line, white, 1));
    out.push_back(text(Font::medium,
                       phrase(RetailString::Wheel_Scroll, false) + " : " +
                           phrase(RetailString::Grab_or_place_items, false),
                       404.0F, 180.0F + line + line, white, 1));
    const PlayerInventory::Slot *chosen = slot(host, selected_);
    if (held_)
        paint.classic_info(*held_, g.background.y + 20);
    else if (chosen != nullptr && chosen->has_value())
        paint.classic_info(**chosen, g.background.y + 20);
    const auto shown = [&](std::size_t index) {
        // With the keyboard, the held stack sits on the selected slot.
        return !(held_ && !pointer_active_ && index == selected_);
    };
    for (std::size_t index = 0; index < backpack_slots; ++index)
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value() && shown(index))
            paint.slot(**entry, *g.draw_rect(index));
    for (std::size_t column = 0; column < 8U; ++column)
    {
        const std::size_t index = tray_base + 8U * g.visible_tray + column;
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value() && shown(index))
            paint.slot(**entry, *g.draw_rect(index));
    }
    const IntRect selector = g.selector_rect(selected_, sprite_size(UiStyle::classic, Sprite::selector));
    if (selector_visible())
        out.push_back(sprite(Sprite::selector, selector, held_ ? red : white));
    if (held_)
    {
        const Point pointer = to_layout(pointer_);
        paint.slot(*held_, pointer_active_ ? IntRect{pointer.x - 29, pointer.y - 29, classic_cell, classic_cell}
                                           : IntRect{selector.x, selector.y, classic_cell, classic_cell});
    }
    paint.button(*button_rect(), phrase(RetailString::Craft, false), true, button_captured_, button_hover_, 1.0F);
}

void Session::draw_classic_crafting(const Host &host, const TextMetrics &metrics, DrawList &out) const
{
    Geometry g = geometry();
    g.visible_tray = host.inventory().current_tray_index();
    g.tray_count = tray_count(host);
    const Painter paint{out, host, metrics, false, 1.0F};
    const auto &book = host.recipes();
    out.push_back(sprite(Sprite::block_ui_back, g.background));
    paint.close_box(close_hover_);
    for (std::size_t index = 0; index < backpack_slots; ++index)
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value())
            paint.slot(**entry, *g.draw_rect(index));
    for (std::size_t column = 0; column < 8U; ++column)
    {
        const std::size_t index = tray_base + 8U * g.visible_tray + column;
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value())
            paint.slot(**entry, *g.draw_rect(index));
    }
    const auto list = strip_recipes(host);
    const int selected = strip_position(host);
    const int left = g.background.x;
    const int top = g.background.y;
    for (int offset = -2; offset < 5; ++offset)
    {
        const int index = selected + offset;
        if (index < 0 || index >= static_cast<int>(list.size()))
            continue;
        const CraftingRecipe &recipe = book[list[static_cast<std::size_t>(index)]];
        const Color tint = host.can_craft(recipe) ? white : dimmed;
        const int x = left + 39;
        const int y = top + 184 + offset * strip_pitch;
        out.push_back(sprite(Sprite::single_grid, {x, y, 76, 76}, tint));
        // The icons slide with the scroll; the frames stay put.
        paint.item(recipe_result_stack(recipe),
                   {x + 4, truncate(static_cast<float>(y) + 4.0F + scroll_offset_), classic_cell, classic_cell}, tint);
    }
    if (recipe_)
    {
        const CraftingRecipe &recipe = book[*recipe_];
        int column = 0;
        std::optional<InventoryStack> chosen_ingredient;
        for (const CraftingIngredient &ingredient : recipe.ingredients)
        {
            if (ingredient.count == 0U)
                continue;
            const int x = left + 131 + column * strip_pitch;
            const int y = top + 184;
            const Color tint = host.ingredient_count(ingredient) >= ingredient.count ? white : dimmed;
            out.push_back(sprite(Sprite::single_grid, {x, y, 76, 76}, tint));
            paint.item(ingredient_stack(ingredient), {x + 4, y + 4, classic_cell, classic_cell},
                       tint);
            if (++column == ingredient_)
                chosen_ingredient = ingredient_stack(ingredient);
        }
        paint.classic_info(chosen_ingredient.value_or(recipe_result_stack(recipe)), top + 20);
        out.push_back(text(Font::medium, phrase(RetailString::Components, false) + ": ", 366.0F, 210.0F, white, 2));
        Point cursor{left + 39, top + 184};
        if (ingredient_ > 0)
            cursor = {left + 131 + (ingredient_ - 1) * strip_pitch, top + 184};
        else if (draw_selector_ != selected)
            cursor = {left + 39, top + 184 + (draw_selector_ - selected) * strip_pitch};
        const Size size = sprite_size(UiStyle::classic, Sprite::selector);
        out.push_back(sprite(Sprite::selector, {cursor.x - 2, cursor.y - 2, size.width, size.height}));
    }
    paint.button(*button_rect(), "Create Item", recipe_ && host.can_craft(book[*recipe_]), button_captured_,
                 button_hover_, 1.0F);
}

void Session::draw_classic_crate(const Host &host, const TextMetrics &metrics, DrawList &out) const
{
    Geometry g = geometry();
    g.visible_tray = host.inventory().current_tray_index();
    g.tray_count = tray_count(host);
    const Painter paint{out, host, metrics, false, 1.0F};
    out.push_back(sprite(Sprite::block_ui_back, g.background));
    const Size grid = sprite_size(UiStyle::classic, Sprite::inventory_grid);
    out.push_back(sprite(Sprite::inventory_grid, {g.background.x + 165, g.background.y + 20, grid.width, grid.height}));
    const auto shown = [&](std::size_t index) { return !(held_ && !pointer_active_ && index == selected_); };
    const auto paint_slot = [&](std::size_t index) {
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value() && shown(index))
            paint.slot(**entry, *g.draw_rect(index));
    };
    for (std::size_t index = 0; index < backpack_slots; ++index)
        paint_slot(index);
    for (std::size_t column = 0; column < 8U; ++column)
        paint_slot(tray_base + 8U * g.visible_tray + column);
    for (std::size_t index = 0; index < crate_slots; ++index)
        paint_slot(crate_base + index);
    const IntRect selector = g.selector_rect(selected_, sprite_size(UiStyle::classic, Sprite::selector));
    if (selector_visible())
        out.push_back(sprite(Sprite::selector, selector, held_ ? red : white));
    if (held_)
    {
        const Point pointer = to_layout(pointer_);
        paint.slot(*held_, pointer_active_ ? IntRect{pointer.x - 29, pointer.y - 29, classic_cell, classic_cell}
                                           : IntRect{selector.x, selector.y, classic_cell, classic_cell});
    }
}

namespace
{

// PlayerInventory.CanConsume: ONE stack must hold the whole amount.
bool one_stack_holds(const PlayerInventory &inventory, InventoryItemId item, std::uint16_t amount) noexcept
{
    const auto holds = [&](const PlayerInventory::Slot &entry) {
        return entry.has_value() && entry->item == item && entry->count >= amount;
    };
    const std::size_t trays = inventory.profile() == InventoryProfile::modern ? 2U : 1U;
    for (std::size_t tray = 0; tray < trays; ++tray)
        for (const PlayerInventory::Slot &entry : inventory.tray(tray))
            if (holds(entry))
                return true;
    for (const PlayerInventory::Slot &entry : inventory.storage())
        if (holds(entry))
            return true;
    return false;
}

Color faded(float alpha) noexcept
{
    const auto byte = static_cast<std::uint8_t>(std::clamp(std::lround(alpha * 255.0F), 0L, 255L));
    return {byte, byte, byte, byte};
}

} // namespace

void Session::draw_modern_inventory(const Host &host, const TextMetrics &metrics, DrawList &out) const
{
    Geometry g = geometry();
    g.tray_count = tray_count(host);
    const double s = g.s;
    const auto fs = static_cast<float>(s); // the scale handed to SpriteBatch
    const Painter paint{out, host, metrics, true, fs};
    const IntRect &back = g.background;
    const auto &book = host.recipes();
    out.push_back(sprite(Sprite::block_ui_back, back));
    out.push_back(marker(Draw::Kind::hud_stats));
    out.push_back(marker(Draw::Kind::hud_distance));
    const auto &groups = modern_recipe_groups();
    const bool tree = recipe_tree_shown();
    std::optional<std::size_t> shown_recipe;
    if (tree)
    {
        static constexpr std::array<Sprite, 4> icons{Sprite::materials_icon, Sprite::tools_icon,
                                                     Sprite::weapons_icon, Sprite::structures_icon};
        for (std::size_t group = 0; group < groups.size(); ++group)
        {
            const IntRect rect = scaled(back, 14.0, group_y[group], 243.0, 36.0, s);
            const bool open = group == group_;
            const Color tint = open ? white : menu_aqua;
            static constexpr std::array<RetailString, 4> titles{RetailString::Materials, RetailString::Tools,
                                                                RetailString::Weapons, RetailString::Structures};
            const std::string title = phrase(titles[group], true);
            out.push_back(text(Font::medium, std::string{title},
                               static_cast<float>(rect.right() - (50.0 + metrics.measure(Font::medium, title).width) * s),
                               static_cast<float>(rect.y), tint, 0, fs));
            const int icon = truncate(35.0 * s);
            out.push_back(sprite(icons[group], {rect.right() - icon, rect.y, icon, icon}, tint));
            if (!open)
                continue;
            out.push_back(sprite(Sprite::craft_selector, rect, tint));
            for (std::size_t type = 0; type < groups[group].type_count; ++type)
            {
                const IntRect tab = scaled(back, 14.0 + 263.0, group_y[group] + 7.0 + 31.0 * static_cast<double>(type),
                                           104.0, 30.0, s);
                const bool chosen = type == type_[group];
                out.push_back(sprite(Sprite::tier2_back, tab));
                out.push_back(text(Font::small, phrase(type_title(groups[group].types[type]), true),
                                   static_cast<float>(tab.x + 10.0 * s), static_cast<float>(tab.y),
                                   chosen ? white : menu_aqua, 0, fs));
                if (!chosen)
                    continue;
                const auto list = type_recipes(host, groups[group].types[type]);
                const std::size_t hovered = list.empty() ? 0U : std::min(recipe_in_type_[group][type], list.size() - 1U);
                for (std::size_t index = 0; index < list.size(); ++index)
                {
                    const IntRect cell = *recipe_rect(host, index);
                    const CraftingRecipe &recipe = book[list[index]];
                    const Color craftable = host.can_craft(recipe) ? white : dimmed;
                    out.push_back(sprite(Sprite::single_grid, cell, craftable));
                    paint.item(recipe_result_stack(recipe),
                               {cell.x, cell.y, truncate(cell.width - 5.0 * s), truncate(cell.height - 5.0 * s)},
                               craftable);
                    if (index != hovered)
                        continue;
                    out.push_back(sprite(Sprite::selector, cell));
                    paint.item(recipe_result_stack(recipe),
                               {back.x + truncate(32.0 * s), back.y + truncate(235.0 * s), truncate(130.0 * s),
                                truncate(130.0 * s)},
                               white, false);
                    shown_recipe = list[index];
                }
                out.push_back(sprite(Sprite::craft_selector,
                                     {tab.x, tab.y, truncate(tab.width - 8.0 * s), truncate(tab.height - 5.0 * s)}));
                if (!shown_recipe)
                    continue;
                int column = 0;
                for (const CraftingIngredient &ingredient : book[*shown_recipe].ingredients)
                {
                    if (ingredient.count == 0U)
                        continue;
                    const int size = truncate(64.0 * s);
                    const IntRect cell{truncate(back.x + (39 + 65 * column) * s), truncate(back.y + 521.0 * s), size, size};
                    out.push_back(sprite(Sprite::single_grid, cell));
                    const int inner = truncate(size - 5.0 * s);
                    // (Polish) A mod item is never its fallback's stack.
                    const bool held = ingredient.mod_item != 0U
                                          ? host.ingredient_count(ingredient) >= ingredient.count
                                          : one_stack_holds(host.inventory(), ingredient.item, ingredient.count);
                    paint.item(ingredient_stack(ingredient), {cell.x, cell.y, inner, inner}, held ? white : dimmed);
                    ++column;
                }
            }
        }
        if (shown_recipe)
        {
            // Three TextRegionElements: the name, the description under it
            // (one MedFont line per name line) and the component names.
            const CraftingRecipe &recipe = book[*shown_recipe];
            const ItemText words = host.item_words(recipe_result_stack(recipe), InventoryProfile::modern);
            int name_lines = 1;
            int other_lines = 1;
            const auto x = static_cast<float>(back.x + 32.0 * s);
            const auto region = static_cast<float>(150.0 * s);
            out.push_back(text(Font::medium, wrap(words.name, Font::medium, region, s, metrics, name_lines), x,
                               static_cast<float>(back.y + 367.0 * s), menu_aqua, 0, fs));
            out.push_back(text(Font::small, wrap(words.description1, Font::small, region, s, metrics, other_lines), x,
                               static_cast<float>(back.y + (367 + metrics.line_spacing(Font::medium) * name_lines) * s),
                               menu_blue, 0, fs));
            std::string parts = phrase(RetailString::Components, true) + ":\n";
            bool first = true;
            for (const CraftingIngredient &ingredient : recipe.ingredients)
            {
                if (ingredient.count == 0U)
                    continue;
                if (!first)
                    parts += ", ";
                parts += host.item_words(ingredient_stack(ingredient), InventoryProfile::modern).name;
                first = false;
            }
            out.push_back(text(Font::small, wrap(parts, Font::small, static_cast<float>(72.0 * s), s, metrics, other_lines),
                               static_cast<float>(back.x + 170.0 * s), static_cast<float>(back.y + 235.0 * s),
                               menu_blue, 0, fs));
        }
    }
    const auto label = [&](std::string_view word, double centre, double top) {
        const std::string caps = upper(word);
        out.push_back(text(Font::small, caps,
                           static_cast<float>(back.x + (centre - metrics.measure(Font::small, caps).width / 2.0) * s),
                           static_cast<float>(back.y + top * s), menu_orange, 1, fs));
    };
    label(phrase(RetailString::Crafting, true), 84.0, 5.0);
    label(phrase(RetailString::Inventory, true), 717.0, 70.0);
    for (std::size_t index = 0; index < backpack_slots; ++index)
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value())
            paint.slot(**entry, *g.draw_rect(index));
    for (std::size_t tray = 0; tray < g.tray_count; ++tray)
    {
        out.push_back(sprite(Sprite::hud_grid, g.trays[tray]));
        for (std::size_t column = 0; column < 8U; ++column)
        {
            const std::size_t index = tray_base + 8U * tray + column;
            if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value())
                paint.slot(**entry, *g.draw_rect(index));
        }
    }
    if (hit_)
        out.push_back(sprite(Sprite::selector, g.selector_rect(selected_, {}), held_ ? red : white));
    if (held_)
    {
        const int size = truncate(g.spacing);
        paint.slot(*held_, {truncate(pointer_.x - static_cast<double>(g.spacing) / 2.0),
                            truncate(pointer_.y - static_cast<double>(g.spacing) / 2.0), size, size});
    }
    // The item-name pop-up.
    const PlayerInventory::Slot *chosen = slot(host, selected_);
    const bool hovering = chosen != nullptr && chosen->has_value() && !held_ && hit_;
    float alpha = -1.0F;
    if (hovering && popup_wait_ >= 0.25F)
        alpha = std::min(1.0F, popup_fade_in_ / 0.25F);
    else if (popup_fade_out_ < popup_fade_out_length_)
        alpha = 1.0F - std::min(1.0F, popup_fade_out_ / popup_fade_out_length_);
    if (alpha >= 0.0F && popup_item_)
    {
        const std::string_view name = host.item_words(*popup_item_, InventoryProfile::modern).name;
        const Extent size = metrics.measure(Font::small, name);
        // Vector2 size = MeasureString(name) * scale
        const double width = static_cast<float>(size.width * s);
        const double height = static_cast<float>(size.height * s);
        Point place{popup_location_.x, popup_location_.y + truncate(height)};
        if (place.y + height > height_)
            place.y -= truncate(height * 2.0);
        if (place.x + width > width_)
            place.x = truncate(width_ - width);
        Color outline = black;
        outline.a = faded(alpha).a;
        out.push_back(text(Font::small, std::string{name}, static_cast<float>(place.x), static_cast<float>(place.y),
                           faded(alpha), 1, fs, outline));
    }
}

void Session::draw_modern_crate(const Host &host, const TextMetrics &metrics, DrawList &out) const
{
    Geometry g = geometry();
    g.tray_count = tray_count(host);
    const double s = g.s;
    const auto fs = static_cast<float>(s); // the scale handed to SpriteBatch
    const Painter paint{out, host, metrics, true, fs};
    const IntRect &back = g.background;
    out.push_back(marker(Draw::Kind::hud_stats));
    out.push_back(marker(Draw::Kind::hud_distance));
    const Size grid = sprite_size(UiStyle::modern, Sprite::inventory_grid);
    const int grid_width = truncate(grid.width * s);
    out.push_back(sprite(Sprite::inventory_grid, {back.x, back.y, grid_width, back.height}));
    out.push_back(sprite(Sprite::inventory_grid, {back.right() - grid_width, back.y, grid_width, back.height}));
    const auto shown = [&](std::size_t index) { return !(held_ && !pointer_active_ && index == selected_); };
    const auto paint_slot = [&](std::size_t index) {
        if (const PlayerInventory::Slot *entry = slot(host, index); entry && entry->has_value() && shown(index))
            paint.slot(**entry, *g.draw_rect(index));
    };
    for (std::size_t index = 0; index < backpack_slots; ++index)
        paint_slot(index);
    for (std::size_t tray = 0; tray < g.tray_count; ++tray)
    {
        out.push_back(sprite(Sprite::hud_grid, g.trays[tray]));
        for (std::size_t column = 0; column < 8U; ++column)
            paint_slot(tray_base + 8U * tray + column);
    }
    const auto label = [&](std::string_view word, double centre) {
        const std::string caps = upper(word);
        out.push_back(text(Font::small, caps,
                           static_cast<float>(back.x + (centre - metrics.measure(Font::small, caps).width / 2.0) * s),
                           static_cast<float>(back.y + 5.0 * s), menu_orange, 1, fs));
    };
    label(phrase(RetailString::Crate, true), 85.0);
    label(phrase(RetailString::Inventory, true), 369.0);
    for (std::size_t index = 0; index < crate_slots; ++index)
        paint_slot(crate_base + index);
    const IntRect selector = g.selector_rect(selected_, {});
    if (!pointer_active_ || hit_)
        out.push_back(sprite(Sprite::selector, selector, held_ ? red : white));
    if (held_)
    {
        const int size = truncate(g.spacing);
        paint.slot(*held_, pointer_active_ ? IntRect{truncate(pointer_.x - static_cast<double>(g.spacing) / 2.0),
                                                     truncate(pointer_.y - static_cast<double>(g.spacing) / 2.0), size, size}
                                           : IntRect{selector.x, selector.y, size, size});
    }
    const PlayerInventory::Slot *chosen = slot(host, selected_);
    const bool hovering = chosen != nullptr && chosen->has_value() && !held_ && hit_;
    float alpha = -1.0F;
    if (hovering && popup_wait_ >= 1.0F)
        alpha = std::min(1.0F, popup_fade_in_ / 0.25F);
    else if (popup_fade_out_ < popup_fade_out_length_)
        alpha = 1.0F - std::min(1.0F, popup_fade_out_ / popup_fade_out_length_);
    if (alpha >= 0.0F && popup_item_)
    {
        const std::string_view name = host.item_words(*popup_item_, InventoryProfile::modern).name;
        const Extent size = metrics.measure(Font::small, name);
        // Vector2 size = MeasureString(name) * scale
        const double width = static_cast<float>(size.width * s);
        const double height = static_cast<float>(size.height * s);
        Point place{popup_location_.x, popup_location_.y + truncate(height)};
        if (place.y + height > height_)
            place.y -= truncate(height * 2.0);
        if (place.x + width > width_)
            place.x = truncate(width_ - width);
        Color outline = black;
        outline.a = faded(alpha).a;
        out.push_back(text(Font::small, std::string{name}, static_cast<float>(place.x), static_cast<float>(place.y),
                           faded(alpha), 1, fs, outline));
    }
}

// -------------------------------------------------------------- remastered
//
// OpenMinerZ's own arrangement (remastered_layout.hpp). Everything from here
// down is hit testing and input routing: which slot, which recipe, which key.
// Whatever changes an inventory is done by the functions above, the ones the
// two retail layouts use, so the three styles cannot disagree about a rule.

namespace
{

namespace rm = openminerz::remastered;

// "All", then 1.9.9.8's four groups in the order CraftingScreen builds them.
bool tab_lists(RecipeTab tab, RecipeType type) noexcept
{
    if (tab == RecipeTab::all)
        return true;
    const RecipeGroup &group = modern_recipe_groups()[static_cast<std::size_t>(tab) - 1U];
    for (std::size_t index = 0; index < group.type_count; ++index)
        if (group.types[index] == type)
            return true;
    return false;
}

} // namespace

std::vector<std::size_t> Session::tab_recipes(const Host &host, RecipeTab tab) const
{
    const InventoryProfile world = host.inventory().profile();
    const auto &book = host.recipes();
    // What the world's own crafting screen lists, in its order: 1.9.9.8 every
    // recipe of every tab, 1.6.3.8 what the player has discovered.
    std::vector<std::size_t> listed;
    if (world == InventoryProfile::modern)
    {
        for (const RecipeGroup &group : modern_recipe_groups())
            for (std::size_t type = 0; type < group.type_count; ++type)
                for (const std::size_t index : type_recipes(host, group.types[type]))
                    listed.push_back(index);
    }
    else
    {
        listed = strip_recipes(host);
    }
    // What can be made right now comes first; the order is otherwise kept.
    std::vector<std::size_t> list;
    for (const bool wants_craftable : {true, false})
        for (const std::size_t index : listed)
            if (tab_lists(tab, recipe_type_for(book[index], world)) &&
                host.can_craft(book[index]) == wants_craftable)
                list.push_back(index);
    return list;
}

std::optional<std::size_t> Session::remastered_recipe(const Host &host) const
{
    if (layout() != Layout::remastered_inventory)
        return std::nullopt;
    const auto list = tab_recipes(host);
    if (list.empty())
        return std::nullopt;
    if (rm_recipe_ && std::find(list.begin(), list.end(), *rm_recipe_) != list.end())
        return rm_recipe_;
    return list.front();
}

int Session::remastered_ingredients(const Host &host) const
{
    const auto recipe = remastered_recipe(host);
    if (!recipe)
        return 0;
    int count = 0;
    for (const CraftingIngredient &ingredient : host.recipes()[*recipe].ingredients)
        count += ingredient.count != 0U ? 1 : 0;
    return count;
}

std::optional<IntRect> Session::tab_rect(RecipeTab tab) const noexcept
{
    if (layout() != Layout::remastered_inventory)
        return std::nullopt;
    const int width = (rm::grid_width - (rm::tab_count - 1) * rm::tab_gap) / rm::tab_count;
    return IntRect{rm::left_x + static_cast<int>(tab) * (width + rm::tab_gap), rm::grid_y, width, rm::tab_height};
}

std::optional<IntRect> Session::recipe_row_rect(std::size_t row) const noexcept
{
    if (layout() != Layout::remastered_inventory || row >= static_cast<std::size_t>(rm::recipe_rows(rm_trays_)))
        return std::nullopt;
    return IntRect{rm::left_x, rm::recipe_list_y + static_cast<int>(row) * rm::recipe_row_height,
                   rm::recipe_list_width - rm::scroll_width - 4, rm::recipe_row_height - 2};
}

std::optional<IntRect> Session::row_craft_rect(std::size_t row) const noexcept
{
    const auto rect = recipe_row_rect(row);
    if (!rect)
        return std::nullopt;
    const int size = rect->height - 8;
    return IntRect{rect->right() - size - 4, rect->y + 4, size, size};
}

std::optional<IntRect> Session::detail_rect() const noexcept
{
    if (layout() != Layout::remastered_inventory)
        return std::nullopt;
    return IntRect{rm::detail_x, rm::recipe_list_y, rm::detail_width,
                   rm::trays_bottom(rm_trays_) - rm::recipe_list_y};
}

std::optional<IntRect> Session::craft_rect(bool all) const noexcept
{
    const auto card = detail_rect();
    if (!card)
        return std::nullopt;
    const int width = (card->width - 20 - 8) / 2;
    return IntRect{card->x + 10 + (all ? width + 8 : 0), card->bottom() - 10 - rm::craft_button_height, width,
                   rm::craft_button_height};
}

void Session::remastered_hover(const Host &host, Point point)
{
    rm_hot_ = Hot::none;
    rm_hot_index_ = -1;
    if (const auto close = close_rect(); close && close->contains(point))
    {
        rm_hot_ = Hot::close;
        return;
    }
    if (layout() != Layout::remastered_inventory)
        return;
    for (std::size_t tab = 0; tab < recipe_tab_count; ++tab)
        if (const auto rect = tab_rect(static_cast<RecipeTab>(tab)); rect && rect->contains(point))
        {
            rm_hot_ = Hot::tab;
            rm_hot_index_ = static_cast<int>(tab);
            return;
        }
    const auto list = tab_recipes(host);
    const auto &book = host.recipes();
    const auto rows = static_cast<std::size_t>(rm::recipe_rows(rm_trays_));
    for (std::size_t row = 0; row < rows && rm_scroll_ + row < list.size(); ++row)
    {
        const auto rect = recipe_row_rect(row);
        if (!rect || !rect->contains(point))
            continue;
        const auto button = row_craft_rect(row);
        rm_hot_ = button && button->contains(point) && host.can_craft(book[list[rm_scroll_ + row]]) ? Hot::row_craft
                                                                                                   : Hot::recipe;
        rm_hot_index_ = static_cast<int>(row);
        return;
    }
    if (!remastered_recipe(host))
        return;
    if (const auto rect = craft_rect(false); rect && rect->contains(point))
    {
        rm_hot_ = Hot::craft_one;
        return;
    }
    if (const auto rect = craft_rect(true); rect && rect->contains(point))
    {
        rm_hot_ = Hot::craft_all;
        return;
    }
    for (int index = 0; index < remastered_ingredients(host); ++index)
        if (const auto rect = ingredient_rect(host, index); rect && rect->contains(point))
        {
            rm_hot_ = Hot::ingredient;
            rm_hot_index_ = index;
            return;
        }
}

void Session::remastered_show_recipe(const Host &host)
{
    const auto list = tab_recipes(host);
    const auto rows = static_cast<std::size_t>(rm::recipe_rows(rm_trays_));
    if (list.empty())
    {
        rm_recipe_.reset();
        rm_scroll_ = 0U;
        return;
    }
    auto found = rm_recipe_ ? std::find(list.begin(), list.end(), *rm_recipe_) : list.end();
    if (found == list.end())
    {
        rm_recipe_ = list.front();
        found = list.begin();
    }
    const auto position = static_cast<std::size_t>(found - list.begin());
    if (position < rm_scroll_)
        rm_scroll_ = position;
    else if (position >= rm_scroll_ + rows)
        rm_scroll_ = position + 1U - rows;
    rm_scroll_ = std::min(rm_scroll_, list.size() > rows ? list.size() - rows : 0U);
}

void Session::set_recipe_tab(const Host &host, RecipeTab tab)
{
    if (static_cast<std::size_t>(tab) >= recipe_tab_count)
        return;
    rm_tab_ = tab;
    rm_scroll_ = 0U;
    remastered_show_recipe(host);
}

void Session::select_recipe(const Host &host, std::size_t recipe_index)
{
    // A recipe the open tab does not list is shown under "All", which lists
    // every recipe the world does.
    const auto here = tab_recipes(host);
    if (std::find(here.begin(), here.end(), recipe_index) == here.end())
    {
        const auto everything = tab_recipes(host, RecipeTab::all);
        if (std::find(everything.begin(), everything.end(), recipe_index) == everything.end())
            return;
        rm_tab_ = RecipeTab::all;
        rm_scroll_ = 0U;
    }
    rm_recipe_ = recipe_index;
    remastered_show_recipe(host);
}

void Session::remastered_step_recipe(Host &host, int direction)
{
    const auto list = tab_recipes(host);
    const auto current = remastered_recipe(host);
    if (list.empty() || !current)
        return;
    const auto position = static_cast<int>(std::find(list.begin(), list.end(), *current) - list.begin());
    const int next = std::clamp(position + direction, 0, static_cast<int>(list.size()) - 1);
    if (next == position)
        return;
    rm_recipe_ = list[static_cast<std::size_t>(next)];
    remastered_show_recipe(host);
    host.play(Cue::click);
}

void Session::remastered_craft(Host &host, std::size_t recipe_index, bool all)
{
    const InventoryProfile world = host.inventory().profile();
    const auto &book = host.recipes();
    // Neither retail crafting screen works with a stack on the cursor
    // (1.6.3.8 hands it back on the way in, 1.9.9.8's Tier2Item ignores the
    // click), so this one does not either.
    if (recipe_index >= book.size() || held_ || !host.can_craft(book[recipe_index]))
    {
        host.play(Cue::error);
        return;
    }
    // "All" means what it means on the world's own screen (craft_every), and
    // the cue comes when that screen plays it.
    const bool modern_world = world == InventoryProfile::modern;
    const bool every = craft_every(host, all);
    if (modern_world)
    {
        host.play(Cue::craft);
        host.craft(recipe_index, every);
    }
    else
    {
        host.craft(recipe_index, every);
        host.play(Cue::craft);
    }
    // Making it re-sorts the list (what can be made comes first): keep the
    // recipe that was just made in view.
    rm_recipe_ = recipe_index;
    remastered_show_recipe(host);
}

void Session::craft_selected(Host &host, bool all)
{
    if (const auto recipe = remastered_recipe(host))
        remastered_craft(host, *recipe, all);
    else if (is_open())
        host.play(Cue::error);
}

void Session::remastered_button(Host &host, Button button, bool shift)
{
    rm_trays_ = static_cast<int>(tray_count(host));
    const Point point = to_layout(pointer_);
    const auto found = slot_at(host, point);
    const bool on_slot = found.has_value();
    if (on_slot)
    {
        pointer_active_ = true;
        hit_ = true;
        selected_ = *found;
        rm_recipes_focused_ = false;
    }
    const bool crate = screen_ == ScreenId::crate;
    const bool outside = !geometry().background.contains(point);
    if (button == Button::right)
    {
        // The retail right button: split a stack or put one item down; outside
        // the panel, throw one item.
        if (on_slot)
        {
            if (crate && selected_locked(host))
                host.play(Cue::error);
            else
                split_or_place_one(host);
        }
        else if (outside && held_)
        {
            throw_held(host, true);
        }
        return;
    }
    // The retail left button: Shift sends the stack across, otherwise pick up
    // or put down; outside the panel, throw the whole stack.
    if (shift && on_slot)
    {
        quick_move(host);
        return;
    }
    if (on_slot)
    {
        if (crate && selected_locked(host))
            host.play(Cue::error);
        else
            pick_or_place(host);
        return;
    }
    if (outside)
    {
        if (held_)
            throw_held(host, false);
        return;
    }
    // A control of the panel itself.
    remastered_hover(host, point);
    const auto list = tab_recipes(host);
    const auto row_recipe = [&]() -> std::optional<std::size_t> {
        const std::size_t entry = rm_scroll_ + static_cast<std::size_t>(std::max(rm_hot_index_, 0));
        return entry < list.size() ? std::optional<std::size_t>{list[entry]} : std::nullopt;
    };
    switch (rm_hot_)
    {
    case Hot::close:
        host.play(Cue::click);
        close(host);
        return;
    case Hot::tab:
        if (static_cast<RecipeTab>(rm_hot_index_) != rm_tab_)
        {
            set_recipe_tab(host, static_cast<RecipeTab>(rm_hot_index_));
            host.play(Cue::click);
        }
        rm_recipes_focused_ = true;
        break;
    case Hot::recipe:
        if (const auto recipe = row_recipe())
        {
            if (rm_recipe_ != recipe)
                host.play(Cue::click);
            rm_recipe_ = recipe;
            rm_recipes_focused_ = true;
        }
        break;
    case Hot::row_craft:
        if (const auto recipe = row_recipe())
        {
            rm_recipes_focused_ = true;
            remastered_craft(host, *recipe, shift);
        }
        break;
    case Hot::craft_one:
        craft_selected(host, shift);
        break;
    case Hot::craft_all:
        craft_selected(host, true);
        break;
    case Hot::ingredient:
    {
        // Jump to the recipe that makes this component, as a click on a
        // component does in 1.6.3.8's crafting screen.
        const auto recipe = remastered_recipe(host);
        const auto &book = host.recipes();
        CraftingIngredient wanted{};
        int seen = 0;
        if (recipe)
            for (const CraftingIngredient &ingredient : book[*recipe].ingredients)
                if (ingredient.count != 0U && seen++ == rm_hot_index_)
                    wanted = ingredient;
        for (const std::size_t index : tab_recipes(host, RecipeTab::all))
            if (book[index].result == wanted.item && book[index].result_mod_item == wanted.mod_item)
            {
                select_recipe(host, index);
                host.play(Cue::click);
                return;
            }
        host.play(Cue::error);
        break;
    }
    case Hot::none:
        break;
    }
    // The list may have moved under the pointer.
    remastered_hover(host, point);
}

void Session::remastered_move(Host &host, int horizontal, int vertical)
{
    if (horizontal == 0 && vertical == 0)
        return;
    pointer_active_ = false;
    rm_keys_moved_ = true;
    rm_hot_ = Hot::none;
    rm_hot_index_ = -1;
    const bool inventory_screen = screen_ == ScreenId::inventory;
    if (inventory_screen && rm_recipes_focused_)
    {
        if (vertical != 0)
        {
            remastered_step_recipe(host, vertical);
        }
        else if (horizontal > 0)
        {
            // Out of the list, into the slots beside it.
            rm_recipes_focused_ = false;
            host.play(Cue::click);
        }
        return;
    }
    // Rows 0..3 are a grid's four rows; rows 4 and 5 the tray rows, which only
    // the backpack side has.
    bool in_crate = selected_ >= crate_base;
    const std::size_t local = in_crate ? selected_ - crate_base : selected_;
    int column = static_cast<int>(local < backpack_slots ? local % 8U : (local - tray_base) % 8U);
    int row = local < backpack_slots ? static_cast<int>(local / 8U) : 4 + static_cast<int>((local - tray_base) / 8U);
    const int rows = in_crate ? 4 : 4 + rm_trays_;
    row = (std::min(row, rows - 1) + vertical + rows) % rows;
    column += horizontal;
    if (column < 0)
    {
        if (inventory_screen)
        {
            // Off the left edge of the slots is the recipe list.
            rm_recipes_focused_ = true;
            remastered_show_recipe(host);
            host.play(Cue::click);
            return;
        }
        in_crate = !in_crate;
        column = 7;
    }
    else if (column > 7)
    {
        if (!inventory_screen)
            in_crate = !in_crate;
        column = 0;
    }
    if (in_crate)
        row = std::min(row, 3);
    const auto cell = static_cast<std::size_t>(column);
    if (in_crate)
        selected_ = crate_base + static_cast<std::size_t>(row) * 8U + cell;
    else if (row < 4)
        selected_ = static_cast<std::size_t>(row) * 8U + cell;
    else
        selected_ = tray_base + static_cast<std::size_t>(row - 4) * 8U + cell;
    host.play(Cue::click);
}

void Session::remastered_key(Host &host, Key key, bool shift)
{
    rm_trays_ = static_cast<int>(tray_count(host));
    const bool inventory_screen = screen_ == ScreenId::inventory;
    const bool in_list = inventory_screen && rm_recipes_focused_;
    // A key acts on the slot the selector shows. With the pointer resting off
    // every slot there is none on screen, and nothing is done to one unseen.
    const bool slot_addressed = !in_list && selector_visible();
    switch (key)
    {
    case Key::up:
    case Key::down:
    case Key::left:
    case Key::right:
        repeat_wait_ = 0.0F;
        repeat_tick_ = 0.0F;
        remastered_move(host, key == Key::left ? -1 : key == Key::right ? 1 : 0,
                        key == Key::up ? -1 : key == Key::down ? 1 : 0);
        // As the retail crate screens: a key press announces the selector.
        if (screen_ == ScreenId::crate)
            host.crate_focus(CrateFocusCause::key, selected_);
        break;
    case Key::enter:
        if (in_list)
            craft_selected(host, shift);
        else if (!slot_addressed)
            break;
        else if (shift)
            quick_move(host);
        else if (selected_locked(host))
            host.play(Cue::error);
        else
            pick_or_place(host);
        break;
    case Key::split:
        if (!slot_addressed)
            break;
        if (selected_locked(host))
            host.play(Cue::error);
        else
            split_or_place_one(host);
        break;
    case Key::transfer:
        if (slot_addressed || held_)
            quick_move(host);
        break;
    case Key::drop:
        if (slot_addressed || held_)
            drop_selected(host);
        break;
    case Key::escape:
    case Key::inventory:
        // Hands a held stack back (what does not fit is dropped) and closes.
        host.play(Cue::click);
        close(host);
        break;
    case Key::switch_pane:
        pointer_active_ = false;
        rm_keys_moved_ = true;
        if (inventory_screen)
        {
            rm_recipes_focused_ = !rm_recipes_focused_;
            if (rm_recipes_focused_)
                remastered_show_recipe(host);
        }
        else if (selected_ >= crate_base)
        {
            selected_ -= crate_base; // the same cell of the backpack
        }
        else
        {
            const std::size_t row = selected_ < backpack_slots ? selected_ / 8U : 3U;
            const std::size_t column = selected_ < backpack_slots ? selected_ % 8U : (selected_ - tray_base) % 8U;
            selected_ = crate_base + row * 8U + column;
        }
        host.play(Cue::click);
        if (screen_ == ScreenId::crate)
            host.crate_focus(CrateFocusCause::key, selected_);
        break;
    case Key::next_category:
    case Key::previous_category:
        if (inventory_screen)
        {
            const int count = static_cast<int>(recipe_tab_count);
            const int next = (static_cast<int>(rm_tab_) + (key == Key::next_category ? 1 : count - 1)) % count;
            set_recipe_tab(host, static_cast<RecipeTab>(next));
            rm_recipes_focused_ = true;
            pointer_active_ = false;
            rm_keys_moved_ = true;
            host.play(Cue::click);
        }
        break;
    }
}

} // namespace openminerz::screens
