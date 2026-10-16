// The `ui` and `input` script libraries (openminerz/modding/ui.hpp).
//
// Client functions need `presentation` (they only draw), so a client-scope UI
// mod works on any server. The functions that also exist on the server -
// ui.toast(player, ...), ui.show_dialogue(player, ...) and the other story
// requests - need `network` there: all they do is send the request to the
// player's client over the mod channel. Because one name serves both sides
// with different arguments, those functions declare no capability and check
// the side's own here.

#include "openminerz/modding/ui.hpp"
#include "service_types.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace openminerz::modding::ui
{

class Api final
{
  public:
    static void install(UiService &service, ApiRegistry &registry, ScriptHost &host);
};

namespace
{

const Value nil_value{};

[[nodiscard]] const Value &arg(std::span<const Value> args, std::size_t index) noexcept
{
    return index < args.size() ? args[index] : nil_value;
}

void require(const CallContext &context, CapabilityMask capability, std::string_view function)
{
    if ((context.capabilities & capability) == 0U)
        throw ScriptError{std::format("{} is not available to this mod here", function)};
}

void require_side(const CallContext &context, std::string_view function)
{
    require(context, context.side == Side::client ? capability::presentation : capability::network, function);
}

[[nodiscard]] Owner owner(const CallContext &context)
{
    return {std::string{context.mod_id}, context.side};
}

[[nodiscard]] Value map_of(std::initializer_list<std::pair<std::string, Value>> fields)
{
    return Value{Map(fields)};
}

// The players a server-side request goes to: one handle, an array of them,
// or nil for everyone in the session.
[[nodiscard]] std::vector<Handle> targets(const Value &value, const std::vector<Handle> &everyone)
{
    if (value.is_nil())
        return everyone;
    if (value.type() == ValueType::handle)
    {
        if (value.as_handle().kind != HandleKind::player)
            throw ScriptError{"expected a player"};
        return {value.as_handle()};
    }
    if (value.type() == ValueType::array)
    {
        std::vector<Handle> result;
        for (const Value &item : value.as_array())
        {
            if (item.type() != ValueType::handle || item.as_handle().kind != HandleKind::player)
                throw ScriptError{"expected an array of players"};
            if (result.size() >= 256U)
                throw ScriptError{"at most 256 players at once"};
            result.push_back(item.as_handle());
        }
        return result;
    }
    throw ScriptError{"expected a player, an array of players, or nil for everyone"};
}

[[nodiscard]] std::string text_of(const Value &value, std::string_view what, std::size_t limit)
{
    if (value.is_nil())
        return {};
    if (value.type() != ValueType::string)
        throw ScriptError{std::format("{} must be a string", what)};
    if (value.as_string().size() > limit)
        throw ScriptError{std::format("{} is longer than {} bytes", what, limit)};
    return value.as_string();
}

// A copy of a client-bound table without anything that cannot travel
// (functions), bounded in depth.
[[nodiscard]] Value plain(const Value &value, std::size_t depth = 0U)
{
    if (depth > 8U)
        throw ScriptError{"the request nests too deeply"};
    switch (value.type())
    {
    case ValueType::callback:
        return {};
    case ValueType::array: {
        Array items;
        for (const Value &item : value.as_array())
            items.push_back(plain(item, depth + 1U));
        return Value{std::move(items)};
    }
    case ValueType::map: {
        Map fields;
        for (const auto &[key, item] : value.as_map())
            if (item.type() != ValueType::callback)
                fields.emplace_back(key, plain(item, depth + 1U));
        return Value{std::move(fields)};
    }
    default:
        return value;
    }
}

void release_all(ScriptHost &host, std::span<const Value> args)
{
    for (const Value &value : args)
        if (value.type() == ValueType::callback)
            host.release(value.as_callback());
}

// Every function a script handed over inside `value`: the service keeps the
// ones a widget adopts and lets the rest go.
void collect(const Value &value, std::vector<Callback> &out, std::size_t depth = 0U)
{
    if (depth > 64U)
        return;
    if (value.type() == ValueType::callback)
        out.push_back(value.as_callback());
    else if (value.type() == ValueType::array)
        for (const Value &item : value.as_array())
            collect(item, out, depth + 1U);
    else if (value.type() == ValueType::map)
        for (const auto &[key, item] : value.as_map())
            collect(item, out, depth + 1U);
}

[[nodiscard]] ParamSpec param(std::string name, ValueType type, std::string doc, bool optional = false)
{
    return {std::move(name), type, optional, std::move(doc)};
}

} // namespace

void Api::install(UiService &service, ApiRegistry &registry, ScriptHost &host)
{
    UiService *self = &service;
    ScriptHost *script = &host;
    const auto client = [&registry](FunctionSpec spec, NativeFunction function) {
        spec.sides = side_client;
        spec.requires_caps = capability::presentation;
        registry.add_function(std::move(spec), std::move(function));
    };
    const auto both = [&registry](FunctionSpec spec, NativeFunction function) {
        spec.sides = side_both;
        spec.requires_caps = capability::none;
        registry.add_function(std::move(spec), std::move(function));
    };
    // A widget the calling mod made, by handle.
    const auto own_widget = [self](const CallContext &context, const Value &value, std::string_view function) {
        if (value.type() != ValueType::handle || value.as_handle().kind != HandleKind::widget)
            throw ScriptError{std::format("{}: expected a widget", function)};
        const Widget *widget = self->widgets_.find(value.as_handle().id);
        if (widget == nullptr)
            throw ScriptError{std::format("{}: the widget no longer exists", function)};
        if (widget->mod != context.mod_id)
            throw ScriptError{std::format("{}: that widget belongs to another mod", function)};
        return value.as_handle().id;
    };

    // ---- Theme -------------------------------------------------------------
    client({"ui.theme",
            "Restyles the game's interface: colours (ui.colors names), fonts (TTF/OTF in the mod), nine-slice "
            "images for panels and buttons, the cursor, the menu sounds and the game's own words. Keys at the top "
            "apply to every interface style; `classic`, `modern` and `remastered` sub-tables apply to one. Calls "
            "merge into this mod's layer; a mod later in the load order wins.",
            {param("theme", ValueType::map, "{colors=, fonts=, images=, cursor=, sounds=, strings=, classic=, modern=, remastered=}")}},
           [self](CallContext &context, std::span<const Value> args) {
               self->theme_.apply(context.mod_id, self->backend_.load_order(context.mod_id), arg(args, 0),
                                  [self](const AssetRef &asset) { return self->backend_.asset_exists(asset); });
               return Value{};
           });
    client({"ui.reset_theme", "Removes everything this mod's ui.theme calls set.", {}},
           [self](CallContext &context, std::span<const Value>) {
               self->theme_.clear(context.mod_id);
               return Value{};
           });
    client({"ui.style", "The interface style in effect: \"classic\", \"modern\" or \"remastered\".", {}, ValueType::string},
           [self](CallContext &, std::span<const Value>) { return Value{std::string{style_name(self->backend_.style())}}; });
    client({"ui.colors", "The names ui.theme and widget colours accept for the game's inks.", {}, ValueType::array},
           [](CallContext &, std::span<const Value>) {
               Array names;
               for (std::size_t index = 0U; index < color_key_count; ++index)
                   names.emplace_back(std::string{color_key_name(static_cast<ColorKey>(index))});
               return Value{std::move(names)};
           });

    // ---- Localization -----------------------------------------------------
    both({"ui.text",
          "This mod's string `key` from lang/<language>.lang (falling back to the base language, then English, then "
          "the key itself), with {name} / {1} placeholders filled from `args`.",
          {param("key", ValueType::string, "a key of the mod's lang files"),
           param("args", ValueType::any, "a table or array of values for the placeholders", true)},
          ValueType::string},
         [self](CallContext &context, std::span<const Value> args) {
             const std::string key = text_of(arg(args, 0), "key", 128U);
             return Value{self->localization_.text(context.mod_id, key, arg(args, 1))};
         });
    both({"ui.language", "The player's language code (\"en\", \"de\", \"pt-br\").", {}, ValueType::string},
         [self](CallContext &, std::span<const Value>) { return Value{self->localization_.language()}; });

    // ---- Screens and widgets ----------------------------------------------
    client({"ui.open_screen",
            "Opens a full screen over the game (and over the game's own menus). {title=, root=<widget> or children={...}, "
            "pause=true, dim=true, closable=true, width=, height=, max_width=1400, on_close=fn, on_back=fn}. A pausing "
            "screen stops the world only in single player. Escape / B closes it unless on_back returns false. Returns "
            "the root widget.",
            {param("screen", ValueType::map, "the screen")},
            ValueType::handle},
           [self](CallContext &context, std::span<const Value> args) {
               return Value{widget_handle(self->open_screen(arg(args, 0), owner(context), false))};
           });
    client({"ui.open_dialog",
            "Opens a modal dialog. With `root` or `children` it is that content in a dialog panel; with `text` and "
            "`buttons = {{text=, on_click=fn, close=true}, ...}` it is a message box whose buttons close it after "
            "their handler. {title=, pause=false, on_close=fn}. Returns the root widget.",
            {param("dialog", ValueType::map, "the dialog")},
            ValueType::handle},
           [self](CallContext &context, std::span<const Value> args) {
               const Value &spec = arg(args, 0);
               if (spec.type() != ValueType::map)
                   throw ScriptError{"ui.open_dialog takes a table"};
               if (spec.field("root") != nullptr || spec.field("children") != nullptr)
                   return Value{widget_handle(self->open_screen(spec, owner(context), true))};
               // A message box: the text, then a row of buttons.
               Array children;
               if (const Value *text = spec.field("text"); text != nullptr && !text->is_nil())
                   children.push_back(map_of({{"kind", Value{"label"}}, {"text", *text}, {"width", Value{"fill"}}}));
               Array buttons;
               std::vector<bool> closes;
               if (const Value *list = spec.field("buttons"); list != nullptr && list->type() == ValueType::array)
               {
                   if (list->as_array().size() > 8U)
                       throw ScriptError{"a dialog has at most 8 buttons"};
                   for (const Value &button : list->as_array())
                   {
                       if (button.type() != ValueType::map)
                           throw ScriptError{"a dialog button is {text=, on_click=fn}"};
                       Map fields{{"kind", Value{"button"}}, {"text", button.field_or("text", Value{"OK"})}};
                       if (const Value *click = button.field("on_click"); click != nullptr && !click->is_nil())
                           fields.emplace_back("on_click", *click);
                       if (const Value *id = button.field("id"); id != nullptr && !id->is_nil())
                           fields.emplace_back("id", *id);
                       closes.push_back(button.field_or("close", Value{true}).type() != ValueType::boolean ||
                                        button.field_or("close", Value{true}).as_bool());
                       buttons.push_back(Value{std::move(fields)});
                   }
               }
               if (buttons.empty())
               {
                   buttons.push_back(map_of({{"kind", Value{"button"}}, {"text", Value{"OK"}}}));
                   closes.push_back(true);
               }
               children.push_back(map_of({{"kind", Value{"stack"}}, {"direction", Value{"horizontal"}}, {"gap", Value{10}},
                                          {"anchor", Value{"right"}}, {"children", Value{std::move(buttons)}}}));
               Map dialog{{"root", map_of({{"kind", Value{"stack"}}, {"gap", Value{16}}, {"stretch", Value{true}},
                                           {"width", Value{std::clamp(spec.field_or("width", Value{460}).as_number(), 200.0, 1200.0)}},
                                           {"children", Value{std::move(children)}}})}};
               for (const char *key : {"title", "pause", "on_close", "on_back", "closable", "dim"})
                   if (const Value *value = spec.field(key); value != nullptr && !value->is_nil())
                       dialog.emplace_back(key, *value);
               const std::uint64_t root = self->open_screen(Value{std::move(dialog)}, owner(context), true);
               // Each button closes the dialog after its own handler.
               if (const Widget *content = self->widgets_.find(root); content != nullptr && !content->children.empty())
                   if (const Widget *row = self->widgets_.find(content->children.back()))
                       for (std::size_t index = 0U; index < row->children.size() && index < closes.size(); ++index)
                           if (Widget *button = self->widgets_.find(row->children[index]); button != nullptr && closes[index])
                               button->action = {InternalAction::Kind::close_screen, 0, {}};
               return Value{widget_handle(root)};
           });
    client({"ui.close", "Closes a screen, dialog or HUD element by its root widget (on_close runs).",
            {param("root", ValueType::handle, "what ui.open_screen / ui.open_dialog / ui.add_hud returned")}},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               const std::uint64_t id = own_widget(context, arg(args, 0), "ui.close");
               const std::uint64_t root = self->widgets_.root_of(id);
               for (auto item = self->hud_.begin(); item != self->hud_.end(); ++item)
                   if ((*item)->root == root)
                   {
                       self->destroy_tree(root);
                       self->hud_.erase(item);
                       return Value{};
                   }
               self->close_screen(root, true);
               return Value{};
           });
    client({"ui.is_open", "Whether a screen, dialog or HUD element is still up.",
            {param("root", ValueType::handle, "its root widget")},
            ValueType::boolean},
           [self](CallContext &, std::span<const Value> args) {
               const Value &value = arg(args, 0);
               return Value{value.type() == ValueType::handle && self->widgets_.find(value.as_handle().id) != nullptr};
           });
    client({"ui.add", "Adds a widget (a table, as in a screen's children) to a panel, stack, grid or list; returns it.",
            {param("parent", ValueType::handle, "a container widget"), param("widget", ValueType::map, "the new widget")},
            ValueType::handle},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               std::vector<Callback> handed;
               collect(arg(args, 1), handed);
               std::vector<Callback> adopted;
               std::uint64_t id{};
               try
               {
                   const std::uint64_t parent = own_widget(context, arg(args, 0), "ui.add");
                   id = self->widgets_.add_child(parent, arg(args, 1), adopted);
               }
               catch (...)
               {
                   self->release(handed);
                   throw;
               }
               for (const Callback &callback : handed)
                   if (std::ranges::find(adopted, callback) == adopted.end())
                       self->release(callback);
               return Value{widget_handle(id)};
           });
    client({"ui.set",
            "Changes a widget's properties (text, value, visible, color, ...). Callbacks it replaces are let go.",
            {param("widget", ValueType::handle, "the widget"), param("properties", ValueType::map, "what to change")}},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               std::vector<Callback> handed;
               collect(arg(args, 1), handed);
               std::vector<Callback> adopted;
               std::vector<Callback> replaced;
               try
               {
                   const std::uint64_t id = own_widget(context, arg(args, 0), "ui.set");
                   self->widgets_.update(id, arg(args, 1), adopted, replaced);
               }
               catch (...)
               {
                   self->release(handed);
                   throw;
               }
               for (const Callback &callback : handed)
                   if (std::ranges::find(adopted, callback) == adopted.end())
                       self->release(callback);
               self->release(replaced);
               return Value{};
           });
    client({"ui.get",
            "Reads a widget's value (\"value\", \"checked\", \"text\", \"visible\", \"enabled\", \"scroll\", \"bounds\", ...).",
            {param("widget", ValueType::handle, "the widget"), param("property", ValueType::string, "its name")},
            ValueType::any},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               const std::uint64_t id = own_widget(context, arg(args, 0), "ui.get");
               return self->widgets_.get(id, text_of(arg(args, 1), "property", 32U));
           });
    client({"ui.remove", "Removes a widget and everything in it.", {param("widget", ValueType::handle, "the widget")}},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               const std::uint64_t id = own_widget(context, arg(args, 0), "ui.remove");
               if (self->widgets_.root_of(id) == id)
                   throw ScriptError{"ui.remove: use ui.close for a screen, dialog or HUD element"};
               self->destroy_tree(id);
               return Value{};
           });
    client({"ui.clear", "Removes every widget inside a container.", {param("widget", ValueType::handle, "the container")}},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               const std::uint64_t id = own_widget(context, arg(args, 0), "ui.clear");
               std::vector<Callback> released;
               self->widgets_.clear_children(id, released);
               self->release(released);
               return Value{};
           });
    client({"ui.find", "The widget with this `id` under a root, or nil.",
            {param("root", ValueType::handle, "where to look"), param("id", ValueType::string, "the widget's id")},
            ValueType::handle},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               const std::uint64_t root = own_widget(context, arg(args, 0), "ui.find");
               const std::optional<std::uint64_t> found =
                   self->widgets_.find_named(root, text_of(arg(args, 1), "id", max_id_bytes));
               return found ? Value{widget_handle(*found)} : Value{};
           });
    client({"ui.focus", "Gives a control on the top screen the keyboard and gamepad focus.",
            {param("widget", ValueType::handle, "a button, toggle, slider or text input")}},
           [self, own_widget](CallContext &context, std::span<const Value> args) {
               const std::uint64_t id = own_widget(context, arg(args, 0), "ui.focus");
               if (UiService::Screen *top = self->top_screen(); top != nullptr && self->widgets_.root_of(id) == top->root)
               {
                   std::vector<Dispatch> dispatches;
                   set_focus(self->widgets_, top->surface, id, dispatches);
                   self->dispatch(dispatches);
               }
               return Value{};
           });

    // ---- HUD -------------------------------------------------------------
    client({"ui.add_hud",
            "Adds an element to the HUD: a widget table anchored to the screen (anchor=\"top_left\" ... "
            "\"bottom_right\", x=, y= in units of the 1280x720 reference), drawn while the game's HUD is. "
            "frame=\"focus\" anchors to the middle 16:9 of a very wide screen; frame=\"game\" to the whole "
            "window without the screen margins, where the Classic and Modern HUDs are drawn (to line up with "
            "their hearts, tray and gauges). Returns the element.",
            {param("element", ValueType::map, "a widget table")},
            ValueType::handle},
           [self](CallContext &context, std::span<const Value> args) {
               return Value{widget_handle(self->add_hud(arg(args, 0), owner(context)))};
           });
    client({"ui.stat_bar",
            "Adds a HUD bar that follows a replicated stat: {stat=\"survival.hunger\", label=, color=, anchor=, x=, y=, "
            "width=160, frame=}. ui.set_stat sets a value locally for a client-only mod.",
            {param("bar", ValueType::map, "the bar")},
            ValueType::handle},
           [self](CallContext &context, std::span<const Value> args) {
               const Value &spec = arg(args, 0);
               if (spec.type() != ValueType::map)
                   throw ScriptError{"ui.stat_bar takes a table"};
               const std::string stat = text_of(spec.field_or("stat", Value{}), "stat", max_id_bytes);
               if (stat.empty())
                   throw ScriptError{"ui.stat_bar needs a stat"};
               Array children;
               if (const Value *label = spec.field("label"); label != nullptr && !label->is_nil())
                   children.push_back(map_of({{"kind", Value{"label"}}, {"text", *label}, {"wrap", Value{false}},
                                              {"outline", Value{true}}}));
               Map bar{{"kind", Value{"progress"}},
                       {"stat", Value{stat}},
                       {"width", spec.field_or("width", Value{160})},
                       {"height", Value{10}}};
               if (const Value *color = spec.field("color"); color != nullptr && !color->is_nil())
                   bar.emplace_back("color", *color);
               children.push_back(Value{std::move(bar)});
               Map element{{"kind", Value{"stack"}},
                           {"direction", Value{"horizontal"}},
                           {"align", Value{"center"}},
                           {"gap", Value{8}},
                           {"padding", Value{Array{Value{8}, Value{5}}}},
                           {"background", Value{"plate"}},
                           {"children", Value{std::move(children)}}};
               for (const char *key : {"anchor", "x", "y", "frame", "id", "visible"})
                   if (const Value *value = spec.field(key); value != nullptr && !value->is_nil())
                       element.emplace_back(key, *value);
               return Value{widget_handle(self->add_hud(Value{std::move(element)}, owner(context)))};
           });
    client({"ui.set_stat", "Sets a stat locally (for client-only mods; replicated stats arrive by themselves).",
            {param("stat", ValueType::string, "its name"), param("value", ValueType::number, "the value"),
             param("max", ValueType::number, "the maximum (1 when nil)", true)}},
           [self](CallContext &, std::span<const Value> args) {
               const std::string name = text_of(arg(args, 0), "stat", max_id_bytes);
               if (name.empty())
                   throw ScriptError{"ui.set_stat needs a stat name"};
               const double value = arg(args, 1).as_number();
               const double maximum = arg(args, 2).is_nil() ? 1.0 : arg(args, 2).as_number();
               if (!std::isfinite(value) || !std::isfinite(maximum))
                   throw ScriptError{"ui.set_stat: numbers must be finite"};
               if (!self->local_stats_.contains(name) && self->local_stats_.size() >= 256U)
                   throw ScriptError{"at most 256 local stats"};
               self->local_stats_.insert_or_assign(name, StatValue{value, maximum});
               return Value{};
           });
    const auto element_arg = [](const Value &value) {
        const std::string name = text_of(value, "element", 32U);
        const std::optional<HudElement> element = parse_hud_element(name);
        if (!element)
            throw ScriptError{std::format("'{}' is not a HUD element (see ui.hud_elements())", name)};
        return *element;
    };
    client({"ui.hide_hud", "Hides one of the game's HUD elements while this session lasts (never saved).",
            {param("element", ValueType::string, "a name from ui.hud_elements()")}},
           [self, element_arg](CallContext &, std::span<const Value> args) {
               self->hud_hidden_[static_cast<std::size_t>(element_arg(arg(args, 0)))] = true;
               return Value{};
           });
    client({"ui.show_hud", "Shows a HUD element ui.hide_hud hid (the player's own HUD settings still apply).",
            {param("element", ValueType::string, "a name from ui.hud_elements()")}},
           [self, element_arg](CallContext &, std::span<const Value> args) {
               self->hud_hidden_[static_cast<std::size_t>(element_arg(arg(args, 0)))] = false;
               return Value{};
           });
    client({"ui.move_hud",
            "Moves one of the game's HUD elements by (dx, dy) units. Only the Remastered HUD is laid out to move; the "
            "Classic and Modern HUDs are exact copies and stay put. Returns whether the current style moves it.",
            {param("element", ValueType::string, "a movable name from ui.hud_elements()"),
             param("dx", ValueType::number, "units right"), param("dy", ValueType::number, "units down")},
            ValueType::boolean},
           [self, element_arg](CallContext &, std::span<const Value> args) {
               const HudElement element = element_arg(arg(args, 0));
               if (!hud_element_movable(element))
                   throw ScriptError{std::format("{} cannot be moved", hud_element_name(element))};
               const double dx = arg(args, 1).as_number();
               const double dy = arg(args, 2).as_number();
               if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > 4000.0 || std::fabs(dy) > 4000.0)
                   throw ScriptError{"ui.move_hud: offsets are -4000..4000 units"};
               self->hud_offsets_[static_cast<std::size_t>(element)] = {static_cast<float>(dx), static_cast<float>(dy)};
               return Value{self->backend_.style() == Style::remastered};
           });
    client({"ui.hud_elements", "The game's HUD elements by name, each {name=, movable=}.", {}, ValueType::array},
           [](CallContext &, std::span<const Value>) {
               Array elements;
               for (std::size_t index = 0U; index < hud_element_count; ++index)
               {
                   const auto element = static_cast<HudElement>(index);
                   elements.push_back(map_of({{"name", Value{std::string{hud_element_name(element)}}},
                                              {"movable", Value{hud_element_movable(element)}}}));
               }
               return Value{std::move(elements)};
           });

    // ---- Notices (client, or server -> player) ----------------------------
    both({"ui.toast",
          "A notice at the right of the HUD. Client: ui.toast(title, text?, {item=, seconds=, color=}). Server: "
          "ui.toast(player|players|nil, title, text?, options?) sends it to players.",
          {param("player_or_title", ValueType::any,
                 "client: the title; server: the player(s), nil for everyone", true),
           param("title_or_text", ValueType::any, "client: the text; server: the title", true),
           param("text_or_options", ValueType::any, "client: {item=, seconds=, color=}; server: the text", true),
           param("options", ValueType::any, "server: {item=, seconds=, color=}", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.toast");
             const std::size_t first = context.side == Side::server ? 1U : 0U;
             Map fields{{"op", Value{"toast"}}, {"title", arg(args, first)}, {"text", arg(args, first + 1U)}};
             if (const Value &options = arg(args, first + 2U); options.type() == ValueType::map)
                 for (const char *key : {"item", "seconds", "color"})
                     if (const Value *value = options.field(key); value != nullptr && !value->is_nil())
                         fields.emplace_back(key, *value);
             const Value message{std::move(fields)};
             if (context.side == Side::client)
                 self->show_toast(message, std::string{context.mod_id});
             else
             {
                 // Fail in the server script, not silently on the client.
                 (void)runs_from_value(arg(args, 1));
                 self->to_players(targets(arg(args, 0), self->session_players()), plain(message));
             }
             return Value{};
         });
    both({"ui.title",
          "A large title and subtitle in the middle of the screen. Client: ui.title(title, subtitle?, {fade_in=, stay=, "
          "fade_out=}). Server: ui.title(player|players|nil, title, subtitle?, options?).",
          {param("player_or_title", ValueType::any,
                 "client: the title; server: the player(s), nil for everyone", true),
           param("title_or_subtitle", ValueType::any, "client: the subtitle; server: the title", true),
           param("subtitle_or_options", ValueType::any,
                 "client: {fade_in=, stay=, fade_out=}; server: the subtitle", true),
           param("options", ValueType::any, "server: {fade_in=, stay=, fade_out=}", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.title");
             const std::size_t first = context.side == Side::server ? 1U : 0U;
             Map fields{{"op", Value{"title"}}, {"title", arg(args, first)}, {"subtitle", arg(args, first + 1U)}};
             if (const Value &options = arg(args, first + 2U); options.type() == ValueType::map)
                 for (const char *key : {"fade_in", "stay", "fade_out"})
                     if (const Value *value = options.field(key); value != nullptr && !value->is_nil())
                         fields.emplace_back(key, *value);
             const Value message{std::move(fields)};
             if (context.side == Side::client)
                 self->show_title(message);
             else
                 self->to_players(targets(arg(args, 0), self->session_players()), plain(message));
             return Value{};
         });
    both({"ui.objective",
          "Adds or updates a line of the objective tracker. Client: ui.objective(id, text, {progress=0..1, done=}). "
          "Server: ui.objective(player|players|nil, id, text, options?).",
          {param("player_or_id", ValueType::any,
                 "client: the objective's id; server: the player(s), nil for everyone", true),
           param("id_or_text", ValueType::any, "client: the text; server: the objective's id", true),
           param("text_or_options", ValueType::any, "client: {progress=0..1, done=}; server: the text", true),
           param("options", ValueType::any, "server: {progress=0..1, done=}", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.objective");
             const std::size_t first = context.side == Side::server ? 1U : 0U;
             Map fields{{"op", Value{"objective"}}, {"id", arg(args, first)}, {"text", arg(args, first + 1U)}};
             if (const Value &options = arg(args, first + 2U); options.type() == ValueType::map)
                 for (const char *key : {"progress", "done"})
                     if (const Value *value = options.field(key); value != nullptr && !value->is_nil())
                         fields.emplace_back(key, *value);
             const Value message{std::move(fields)};
             if (context.side == Side::client)
                 self->set_objective(message, std::string{context.mod_id}, Side::client);
             else
                 self->to_players(targets(arg(args, 0), self->session_players()), plain(message));
             return Value{};
         });
    both({"ui.clear_objective",
          "Removes an objective (nil: all of this mod's). Server: ui.clear_objective(player|players|nil, id?).",
          {param("player_or_id", ValueType::any, "client: the id; server: the player(s)", true),
           param("id", ValueType::any, "server: the id", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.clear_objective");
             if (context.side == Side::client)
             {
                 const std::string id = text_of(arg(args, 0), "id", max_id_bytes);
                 std::erase_if(self->objectives_, [&](const auto &objective) {
                     return !objective->from_server && objective->mod == context.mod_id &&
                            (id.empty() || objective->id == id);
                 });
             }
             else
                 self->to_players(targets(arg(args, 0), self->session_players()),
                                  map_of({{"op", Value{"clear_objective"}},
                                          {"id", Value{text_of(arg(args, 1), "id", max_id_bytes)}}}));
             return Value{};
         });

    // ---- Story -------------------------------------------------------------
    both({"ui.show_dialogue",
          "An NPC conversation at the foot of the screen: {id=, speaker=, text=, portrait_item=, choices={{id=, text=}, "
          "...}, pause=true}. Client: each choice may have on_select=fn, or give on_choice=fn(choice_id). Server: "
          "ui.show_dialogue(player, dialogue, on_choice?) - the answer comes back as on_choice(player, choice_id) and "
          "as the ui.dialogue_choice event (several players: use the event).",
          {param("player_or_dialogue", ValueType::any,
                 "client: the dialogue; server: the player(s), nil for everyone", true),
           param("dialogue", ValueType::any, "server: the dialogue", true),
           param("on_choice", ValueType::callback, "server: called with (player, choice)", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self, script](CallContext &context, std::span<const Value> args) {
             try
             {
                 require_side(context, "ui.show_dialogue");
             }
             catch (...)
             {
                 release_all(*script, args);
                 throw;
             }
             if (context.side == Side::client)
             {
                 // The dialogue keeps on_choice and the choices' on_select.
                 std::vector<Callback> handed;
                 collect(arg(args, 0), handed);
                 try
                 {
                     self->open_dialogue(arg(args, 0), owner(context), false);
                 }
                 catch (...)
                 {
                     self->release(handed);
                     throw;
                 }
                 const UiService::Screen *screen = self->top_screen();
                 for (const Callback &callback : handed)
                     if (screen == nullptr ||
                         (callback != screen->on_choice &&
                          std::ranges::find(screen->choice_callbacks, callback) == screen->choice_callbacks.end()))
                         self->release(callback);
                 return Value{};
             }
             const Value &spec = arg(args, 1);
             Callback on_choice;
             if (arg(args, 2).type() == ValueType::callback)
                 on_choice = arg(args, 2).as_callback();
             std::vector<Callback> stray;
             collect(spec, stray);
             self->release(stray); // functions cannot travel to a client
             try
             {
                 if (spec.type() != ValueType::map)
                     throw ScriptError{"ui.show_dialogue(player, dialogue): the dialogue is a table"};
                 const std::string id = text_of(spec.field_or("id", Value{}), "id", max_id_bytes);
                 if (id.empty())
                     throw ScriptError{"a server dialogue needs an id"};
                 const std::vector<Handle> players = targets(arg(args, 0), self->session_players());
                 if (on_choice.valid() && players.size() != 1U)
                     throw ScriptError{"on_choice needs exactly one player; use the ui.dialogue_choice event"};
                 const Value message = plain(spec);
                 Map fields = message.as_map();
                 fields.emplace_back("op", Value{"dialogue"});
                 // The choices the player may answer with, as the client will number them.
                 std::vector<std::string> choices;
                 if (const Value *list = spec.field("choices"); list != nullptr && list->type() == ValueType::array)
                 {
                     if (list->as_array().size() > max_dialogue_choices)
                         throw ScriptError{std::format("a dialogue has at most {} choices", max_dialogue_choices)};
                     for (std::size_t index = 0U; index < list->as_array().size(); ++index)
                     {
                         const Value &choice = list->as_array()[index];
                         std::string choice_id = choice.type() == ValueType::map
                                                     ? text_of(choice.field_or("id", Value{}), "choice id", max_id_bytes)
                                                     : std::string{};
                         choices.push_back(choice_id.empty() ? std::to_string(index + 1U) : choice_id);
                     }
                 }
                 const Value request{std::move(fields)};
                 for (const Handle &player : players)
                 {
                     auto &open = self->server_dialogues_[player.id];
                     std::erase_if(open, [&](const auto &dialogue) {
                         if (dialogue->id != id)
                             return false;
                         self->release(dialogue->callback);
                         return true;
                     });
                     if (open.size() >= 8U)
                     {
                         self->release(open.front()->callback);
                         open.erase(open.begin());
                     }
                     auto dialogue = std::make_unique<UiService::ServerDialogue>();
                     dialogue->id = id;
                     dialogue->mod = std::string{context.mod_id};
                     dialogue->choices = choices.empty() ? std::vector<std::string>{"1"} : choices;
                     dialogue->callback = on_choice;
                     open.push_back(std::move(dialogue));
                 }
                 self->to_players(players, request);
             }
             catch (...)
             {
                 self->release(on_choice);
                 throw;
             }
             return Value{};
         });
    both({"ui.close_dialogue", "Closes a dialogue. Client: ui.close_dialogue(id?). Server: ui.close_dialogue(player, id).",
          {param("player_or_id", ValueType::any, "client: the id; server: the player(s)", true),
           param("id", ValueType::string, "server: the id", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.close_dialogue");
             if (context.side == Side::client)
             {
                 self->close_dialogue(text_of(arg(args, 0), "id", max_id_bytes));
                 return Value{};
             }
             const std::string id = text_of(arg(args, 1), "id", max_id_bytes);
             const std::vector<Handle> players = targets(arg(args, 0), self->session_players());
             for (const Handle &player : players)
                 if (auto found = self->server_dialogues_.find(player.id); found != self->server_dialogues_.end())
                     std::erase_if(found->second, [&](const auto &dialogue) {
                         if (dialogue->id != id)
                             return false;
                         self->release(dialogue->callback);
                         return true;
                     });
             self->to_players(players, map_of({{"op", Value{"close_dialogue"}}, {"id", Value{id}}}));
             return Value{};
         });
    both({"ui.set_journal",
          "Replaces the quest journal: {{id=, title=, text=, state=\"active\"|\"done\"|\"failed\", objectives={{text=, "
          "done=, progress=}, ...}}, ...}. Server: ui.set_journal(player|players|nil, quests).",
          {param("player_or_quests", ValueType::any,
                 "client: the quests; server: the player(s), nil for everyone", true),
           param("quests", ValueType::any, "server: the quests", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.set_journal");
             if (context.side == Side::client)
                 self->set_journal(arg(args, 0));
             else
                 self->to_players(targets(arg(args, 0), self->session_players()),
                                  map_of({{"op", Value{"journal"}}, {"quests", plain(arg(args, 1))}}));
             return Value{};
         });
    both({"ui.open_journal", "Opens the quest journal. Server: ui.open_journal(player|players|nil).",
          {param("player", ValueType::any, "server: the player(s)", true)},
          ValueType::nil, {}, side_both, capability::none, true},
         [self](CallContext &context, std::span<const Value> args) {
             require_side(context, "ui.open_journal");
             if (context.side == Side::client)
                 self->open_journal();
             else
                 self->to_players(targets(arg(args, 0), self->session_players()), map_of({{"op", Value{"open_journal"}}}));
             return Value{};
         });

    // ---- Menus -------------------------------------------------------------
    client({"ui.add_menu_entry",
            "Adds a row to the main menu (before Exit), the in-game menu (before Main Menu) or the Add-Ons screen: "
            "{menu=\"main\"|\"pause\"|\"addons\", label=, description=, on_select=fn}. Returns its id.",
            {param("entry", ValueType::map, "the row")},
            ValueType::integer},
           [self](CallContext &context, std::span<const Value> args) {
               const Value &spec = arg(args, 0);
               Callback on_select;
               if (spec.type() == ValueType::map)
                   if (const Value *select = spec.field("on_select"); select != nullptr && select->type() == ValueType::callback)
                       on_select = select->as_callback();
               try
               {
                   if (spec.type() != ValueType::map)
                       throw ScriptError{"ui.add_menu_entry takes a table"};
                   const std::optional<MenuSlot> slot = parse_menu_slot(text_of(spec.field_or("menu", Value{"main"}), "menu", 16U));
                   if (!slot)
                       throw ScriptError{"menu is \"main\", \"pause\" or \"addons\""};
                   const std::string label = text_of(spec.field_or("label", Value{}), "label", 64U);
                   if (label.empty())
                       throw ScriptError{"a menu entry needs a label"};
                   if (std::ranges::count_if(self->menu_items_, [&](const auto &item) { return item->slot == *slot; }) >= 8)
                       throw ScriptError{"at most 8 add-on rows per menu"};
                   auto item = std::make_unique<MenuItem>();
                   item->id = self->next_menu_id_++;
                   item->slot = *slot;
                   item->label = label;
                   item->description = text_of(spec.field_or("description", Value{}), "description", 256U);
                   item->mod = std::string{context.mod_id};
                   item->callback = on_select;
                   const std::uint64_t id = item->id;
                   self->menu_items_.push_back(std::move(item));
                   ++self->menu_revision_;
                   return Value{static_cast<std::int64_t>(id)};
               }
               catch (...)
               {
                   self->release(on_select);
                   throw;
               }
           });
    client({"ui.remove_menu_entry", "Removes a row ui.add_menu_entry added.",
            {param("id", ValueType::integer, "what ui.add_menu_entry returned")}},
           [self](CallContext &context, std::span<const Value> args) {
               const auto id = static_cast<std::uint64_t>(arg(args, 0).as_integer());
               for (auto item = self->menu_items_.begin(); item != self->menu_items_.end(); ++item)
                   if ((*item)->id == id && (*item)->mod == context.mod_id)
                   {
                       self->release((*item)->callback);
                       self->menu_items_.erase(item);
                       ++self->menu_revision_;
                       break;
                   }
               return Value{};
           });

    // ---- Input -------------------------------------------------------------
    client({"input.bind",
            "Gives the action \"<mod>.<action>\" a default key and a handler run when it is pressed in the world. A key "
            "the game uses, or one an earlier mod has, is never taken: the binding is listed as in conflict until the "
            "player gives it a key (Options > Controls, or Add-Ons > Add-On Key Bindings). {label=} names it there.",
            {param("action", ValueType::string, "a name for the action"),
             param("key", ValueType::string, "the default key (\"J\", \"F6\", \"Keypad 1\"), or nil for none", true),
             param("handler", ValueType::callback, "called with the action's name"),
             param("options", ValueType::map, "{label=}", true)}},
           [self](CallContext &context, std::span<const Value> args) {
               Callback handler;
               if (arg(args, 2).type() == ValueType::callback)
                   handler = arg(args, 2).as_callback();
               try
               {
                   if (!handler.valid())
                       throw ScriptError{"input.bind needs a handler function"};
                   const std::string action = text_of(arg(args, 0), "action", 48U);
                   const std::string key = text_of(arg(args, 1), "key", 32U);
                   const std::string label = arg(args, 3).type() == ValueType::map
                                                 ? text_of(arg(args, 3).field_or("label", Value{}), "label", 96U)
                                                 : std::string{};
                   const Callback replaced = self->bindings_.bind(context.mod_id, self->backend_.load_order(context.mod_id),
                                                                  action, key, label, handler);
                   self->release(replaced);
                   ++self->menu_revision_;
               }
               catch (...)
               {
                   self->release(handler);
                   throw;
               }
               return Value{};
           });
    client({"input.unbind", "Removes one of this mod's actions.", {param("action", ValueType::string, "its name")}},
           [self](CallContext &context, std::span<const Value> args) {
               const std::string action = std::format("{}.{}", context.mod_id, text_of(arg(args, 0), "action", 48U));
               self->release(self->bindings_.unbind(action));
               ++self->menu_revision_;
               return Value{};
           });
    client({"input.key", "The key one of this mod's actions has now, or nil.",
            {param("action", ValueType::string, "its name")},
            ValueType::string},
           [self](CallContext &context, std::span<const Value> args) {
               const std::string action = std::format("{}.{}", context.mod_id, text_of(arg(args, 0), "action", 48U));
               const KeyBinding *binding = self->bindings_.find(action);
               return binding != nullptr && !binding->key.empty() ? Value{binding->key} : Value{};
           });

    // ---- Events ------------------------------------------------------------
    registry.add_event({"ui.dialogue_choice",
                        "A player answered a dialogue the server showed them.",
                        side_server,
                        false,
                        {param("player", ValueType::handle, "who answered"), param("dialogue", ValueType::string, "its id"),
                         param("choice", ValueType::string, "the choice's id")}});
    registry.add_event({"ui.dialogue_closed",
                        "A player closed a server dialogue without answering.",
                        side_server,
                        false,
                        {param("player", ValueType::handle, "who"), param("dialogue", ValueType::string, "its id")}});
    registry.add_event({"ui.screen_closed",
                        "A screen or dialog this side opened was closed (by the player or ui.close).",
                        side_client,
                        false,
                        {param("screen", ValueType::handle, "its root widget")}});

    // A side's VM going away takes everything it made with it. The listener
    // cannot be removed, so it checks that the service still exists.
    host.add_teardown_listener([self, alive = std::weak_ptr<int>{service.lifetime_}](Side side) {
        if (!alive.expired())
            self->clear_side(side);
    });
}

void UiService::register_api(ApiRegistry &registry, ScriptHost &host)
{
    host_ = &host;
    Api::install(*this, registry, host);
}

void register_ui_api(ApiRegistry &registry, ScriptHost &host, UiService &service)
{
    service.register_api(registry, host);
}

} // namespace openminerz::modding::ui
