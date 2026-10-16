// The API reference, Luau type definitions and JSON dump (reference.hpp).

#include "reference.hpp"

#include "openminerz/inventory.hpp"
#include "openminerz/modding/gameplay.hpp"
#include "openminerz/modding/runtime.hpp"
#include "openminerz/net/mod_handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <set>

namespace openminerz::mod_sdk
{

using modding::ApiRegistry;
using modding::CapabilityMask;
using modding::EventSpec;
using modding::FunctionSpec;
using modding::ParamSpec;
using modding::SideMask;
using modding::ValueType;

namespace
{

struct LibraryInfo final
{
    std::string_view name;
    std::string_view area;
    std::string_view summary;
};

// One line per library for the contents table. The reference itself comes
// from the registry; a library missing here is still documented, just
// without a summary.
constexpr LibraryInfo libraries[]{
    {"events", "runtime", "Listen to engine events; emit and listen to your own"},
    {"log", "runtime", "Write to the game log, tagged with your mod's id"},
    {"timer", "runtime", "Run a function later, or every few seconds"},
    {"storage", "runtime", "Save data per world, per player or per machine"},
    {"mod", "runtime", "Your mod's id, version, side, settings and what it may call"},
    {"util", "runtime", "Deterministic random numbers and JSON serialization"},
    {"world", "gameplay", "Read and edit blocks, raycasts, biomes, light"},
    {"worldgen", "gameplay", "Decorate newly generated chunks (ores, ruins, dungeons)"},
    {"terrain", "world", "Replace the world generator: biomes, layout, caves, ores, trees"},
    {"noise", "world", "Noise expressions the world generator evaluates natively"},
    {"structures", "gameplay", "Define, parse and place block structures"},
    {"blocks", "gameplay", "Define new blocks (with a retail fallback)"},
    {"items", "gameplay", "Define new items (with a retail fallback)"},
    {"recipes", "gameplay", "Add, remove and run crafting recipes"},
    {"loot", "gameplay", "Loot tables for crates, enemies and drops"},
    {"creatures", "gameplay", "Define creatures and NPCs and how they spawn"},
    {"entities", "gameplay", "Spawn and control creatures and NPCs"},
    {"players", "gameplay", "Players: health, position, inventory, messages"},
    {"stats", "gameplay", "Per-player stats such as hunger and thirst"},
    {"effects", "gameplay", "Timed status effects (poison, well fed, ...)"},
    {"quests", "gameplay", "Multi-stage quests with objectives"},
    {"dialogue", "gameplay", "NPC dialogue trees"},
    {"commands", "gameplay", "Chat commands (/name)"},
    {"time", "gameplay", "Day counter and time of day"},
    {"difficulty", "gameplay", "Enemy health, damage and spawn scaling"},
    {"ui", "ui", "Themes, widgets, screens, HUD, toasts, menus, dialogue"},
    {"input", "ui", "Key bindings for mod actions"},
    {"light", "graphics", "Coloured point and spot lights"},
    {"fx", "graphics", "Particle effects"},
    {"model", "graphics", "glTF models placed in the world"},
    {"atmosphere", "graphics", "Fog, sky, ambient light and colour grading layers"},
    {"sound", "graphics", "Play sounds (2D or positioned)"},
    {"music", "graphics", "Play and crossfade music"},
    {"camera", "graphics", "Camera shake, fades and field-of-view pulses"},
    {"assets", "graphics", "Ask which asset files exist"},
    {"gunfx", "graphics", "Weapon parts, clip cues and first-person weapon motion"}, // (GunFX)
    {"net", "network", "Messages between the server and clients (RPC)"},
    {"replicated", "network", "Server-owned state every client can read"},
};

[[nodiscard]] const LibraryInfo *library_info(std::string_view name) noexcept
{
    for (const LibraryInfo &info : libraries)
        if (info.name == name)
            return &info;
    return nullptr;
}

[[nodiscard]] std::string_view doc_type(ValueType type) noexcept
{
    switch (type)
    {
    case ValueType::nil:
        return "nil";
    case ValueType::boolean:
        return "boolean";
    case ValueType::integer:
        return "integer";
    case ValueType::number:
        return "number";
    case ValueType::string:
        return "string";
    case ValueType::vec3:
        return "vector";
    case ValueType::handle:
        return "Handle";
    case ValueType::callback:
        return "function";
    case ValueType::array:
        return "array";
    case ValueType::map:
        return "table";
    case ValueType::any:
        return "any";
    }
    return "any";
}

[[nodiscard]] std::string luau_type(ValueType type, bool parameter)
{
    switch (type)
    {
    case ValueType::nil:
        return "nil";
    case ValueType::boolean:
        return "boolean";
    case ValueType::integer:
    case ValueType::number:
        return "number";
    case ValueType::string:
        return "string";
    case ValueType::vec3:
        // A parameter declared vec3 also accepts a {x=, y=, z=} table.
        return parameter ? "Vec3Like" : "vector";
    case ValueType::handle:
        return "Handle";
    case ValueType::callback:
        return "(...any) -> ...any";
    case ValueType::array:
        return "{ any }";
    case ValueType::map:
        return "{ [any]: any }";
    case ValueType::any:
        return "any";
    }
    return "any";
}

[[nodiscard]] bool mentions_nil(std::string_view text) noexcept
{
    for (std::size_t at = text.find("nil"); at != std::string_view::npos; at = text.find("nil", at + 1U))
    {
        const bool start = at == 0U || std::isalnum(static_cast<unsigned char>(text[at - 1U])) == 0;
        const bool end = at + 3U >= text.size() || std::isalnum(static_cast<unsigned char>(text[at + 3U])) == 0;
        if (start && end)
            return true;
    }
    return false;
}

[[nodiscard]] std::string luau_return(const FunctionSpec &spec)
{
    if (spec.returns == ValueType::nil)
        return "()";
    std::string type = luau_type(spec.returns, false);
    if (spec.returns == ValueType::any)
        return type;
    if (spec.returns == ValueType::callback)
        type = "(" + type + ")";
    return mentions_nil(spec.returns_doc) ? type + "?" : type;
}

[[nodiscard]] std::string luau_param_type(const ParamSpec &param)
{
    std::string type = luau_type(param.type, true);
    if (!param.optional || param.type == ValueType::any)
        return type;
    if (param.type == ValueType::callback)
        return "((...any) -> ...any)?";
    return type + "?";
}

// "player.damaged" -> "PlayerDamagedEvent"
[[nodiscard]] std::string event_type_name(std::string_view event)
{
    std::string name;
    bool upper = true;
    for (const char character : event)
    {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0)
        {
            upper = true;
            continue;
        }
        name += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(character))) : character;
        upper = false;
    }
    return name + "Event";
}

[[nodiscard]] std::string escape_cell(std::string_view text)
{
    std::string result;
    for (const char character : text)
    {
        if (character == '|')
            result += "\\|";
        else if (character == '\n')
            result += "<br>";
        else
            result += character;
    }
    return result;
}

[[nodiscard]] std::string json_string(std::string_view text)
{
    std::string result{"\""};
    for (const char character : text)
    {
        switch (character)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U)
                result += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(character)));
            else
                result += character;
        }
    }
    return result + "\"";
}

[[nodiscard]] std::string signature(const FunctionSpec &spec)
{
    std::string text = spec.name + "(";
    for (std::size_t index = 0U; index < spec.params.size(); ++index)
    {
        const ParamSpec &param = spec.params[index];
        text += std::format("{}{}: {}{}", index == 0U ? "" : ", ", param.name, doc_type(param.type),
                            param.optional ? "?" : "");
    }
    if (spec.variadic)
        text += spec.params.empty() ? "..." : ", ...";
    text += ")";
    if (spec.returns != ValueType::nil)
        text += std::format(" -> {}", doc_type(spec.returns));
    return text;
}

[[nodiscard]] std::string availability_text(const FunctionSpec &spec)
{
    const Availability usable = availability(spec);
    std::vector<std::string_view> who;
    if (usable.client_mod)
        who.push_back("client mods");
    if (usable.server_mod_server && usable.server_mod_client)
        who.push_back("server mods (both VMs)");
    else if (usable.server_mod_server)
        who.push_back("server mods (server VM)");
    else if (usable.server_mod_client)
        who.push_back("server mods (client VM)");
    if (who.empty())
        return "nobody (check the registration)";
    std::string text;
    for (std::size_t index = 0U; index < who.size(); ++index)
        text += std::format("{}{}", index == 0U ? "" : ", ", who[index]);
    return text;
}

// Nested library tables of the Luau definitions: storage.world.get is the
// function `get` of table `world` of global `storage`.
struct LuauTable final
{
    std::map<std::string, LuauTable> tables;
    std::map<std::string, const FunctionSpec *> functions;
    std::map<std::string, const modding::ConstantSpec *> constants;
};

void write_docs(std::string &out, std::string_view doc, std::string_view indent)
{
    std::size_t start{};
    while (start < doc.size())
    {
        // Wrap near 96 columns at spaces.
        std::size_t end = std::min(doc.size(), start + 96U);
        if (end < doc.size())
        {
            const std::size_t space = doc.rfind(' ', end);
            if (space != std::string_view::npos && space > start)
                end = space;
        }
        out += std::format("{}--- {}\n", indent, doc.substr(start, end - start));
        start = end;
        while (start < doc.size() && doc[start] == ' ')
            ++start;
    }
}

[[nodiscard]] std::string luau_function_type(const FunctionSpec &spec, const ApiRegistry &registry)
{
    // events.on / once: one overload per engine event, so editors complete
    // the event name and know its payload; mod events fall back to `any`.
    if (spec.name == "events.on" || spec.name == "events.once")
    {
        std::string type;
        for (const auto &[name, event] : registry.events())
            type += std::format("((name: \"{}\", handler: (event: {}) -> boolean?) -> ()) & ", name,
                                event_type_name(name));
        return type + "((name: string, handler: (event: any) -> boolean?) -> ())";
    }
    std::string type = "(";
    for (std::size_t index = 0U; index < spec.params.size(); ++index)
    {
        const ParamSpec &param = spec.params[index];
        type += std::format("{}{}: {}", index == 0U ? "" : ", ", param.name, luau_param_type(param));
    }
    if (spec.variadic)
        type += spec.params.empty() ? "...any" : ", ...any";
    return type + ") -> " + luau_return(spec);
}

void write_luau_table(std::string &out, const LuauTable &table, const ApiRegistry &registry,
                      const std::string &indent)
{
    for (const auto &[name, function] : table.functions)
    {
        write_docs(out, function->doc, indent);
        for (const ParamSpec &param : function->params)
            if (!param.doc.empty())
                write_docs(out, std::format("@param {} {}", param.name, param.doc), indent);
        if (!function->returns_doc.empty())
            write_docs(out, "@return " + function->returns_doc, indent);
        write_docs(out,
                   std::format("Runs on: {}. Usable by: {}.", side_text(function->sides), availability_text(*function)),
                   indent);
        out += std::format("{}{}: {},\n", indent, name, luau_function_type(*function, registry));
    }
    for (const auto &[name, constant] : table.constants)
    {
        write_docs(out, constant->doc, indent);
        out += std::format("{}{}: {},\n", indent, name, luau_type(constant->value.type(), false));
    }
    for (const auto &[name, nested] : table.tables)
    {
        out += std::format("{}{}: {{\n", indent, name);
        write_luau_table(out, nested, registry, indent + "    ");
        out += indent + "},\n";
    }
}

[[nodiscard]] std::string generate_luau(const ApiRegistry &registry)
{
    std::string out;
    out += "-- OpenMinerZ modding API, type definitions for luau-lsp and other Luau tools.\n";
    out += std::format("-- API version {}. Generated by `openminerz_mod_sdk reference` from the game's own\n",
                       modding::api_version);
    out += "-- registry: do not edit. Point luau-lsp at it with \"luau-lsp.types.definitionFiles\"\n";
    out += "-- (see docs/modding/authoring.md).\n\n";
    out += "--- An engine object: a player, entity, widget, light, effect, sound, timer or quest.\n";
    out += "--- The same object is always the same (==) handle, so handles work as table keys.\n";
    out += "declare class Handle\n";
    out += "    --- \"player\", \"entity\", \"widget\", \"light\", \"effect\", \"sound\", \"timer\", \"quest\", ...\n";
    out += "    kind: string\n";
    out += "    --- Never reused within a session.\n";
    out += "    id: number\n";
    out += "    valid: boolean\n";
    out += "end\n\n";
    out += "--- Anything a vector parameter accepts: vector.create(x, y, z) or {x = , y = , z = }.\n";
    out += "type Vec3Like = vector | { x: number, y: number, z: number }\n\n";

    // Event payloads.
    for (const auto &[name, event] : registry.events())
    {
        write_docs(out, std::format("Payload of \"{}\" ({}{}). {}", name, side_text(event.sides),
                                    event.cancellable ? ", cancellable: return false to cancel" : "", event.doc),
                   "");
        out += std::format("type {} = {{\n", event_type_name(name));
        for (const ParamSpec &field : event.payload)
        {
            if (!field.doc.empty())
                write_docs(out, field.doc, "    ");
            out += std::format("    {}: {},\n", field.name,
                               field.optional && field.type != ValueType::any
                                   ? (field.type == ValueType::callback ? "((...any) -> ...any)?"
                                                                        : luau_type(field.type, false) + "?")
                                   : luau_type(field.type, false));
        }
        out += "}\n\n";
    }

    // Libraries.
    std::map<std::string, LuauTable> globals;
    for (const auto &[name, function] : registry.functions())
    {
        std::vector<std::string> segments;
        std::size_t start{};
        for (std::size_t dot = name.find('.'); ; dot = name.find('.', start))
        {
            segments.push_back(name.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos)
                break;
            start = dot + 1U;
        }
        if (segments.size() < 2U || reserved_global(segments.front()))
            continue;
        LuauTable *table = &globals[segments.front()];
        for (std::size_t index = 1U; index + 1U < segments.size(); ++index)
            table = &table->tables[segments[index]];
        table->functions[segments.back()] = &function.spec;
    }
    for (const auto &[name, constant] : registry.constants())
    {
        const std::size_t dot = name.find('.');
        if (dot == std::string::npos || reserved_global(name.substr(0U, dot)))
            continue;
        globals[name.substr(0U, dot)].constants[name.substr(dot + 1U)] = &constant;
    }
    for (const auto &[name, table] : globals)
    {
        if (const LibraryInfo *info = library_info(name))
            write_docs(out, info->summary, "");
        out += std::format("declare {}: {{\n", name);
        write_luau_table(out, table, registry, "    ");
        out += "}\n\n";
    }
    while (out.ends_with("\n\n"))
        out.pop_back();
    return out;
}

[[nodiscard]] std::string generate_markdown(const ApiRegistry &registry)
{
    std::map<std::string, std::vector<const FunctionSpec *>> by_library;
    for (const auto &[name, function] : registry.functions())
        by_library[std::string{library_of(name)}].push_back(&function.spec);
    std::map<std::string, std::vector<const EventSpec *>> events_by_group;
    for (const auto &[name, event] : registry.events())
        events_by_group[std::string{library_of(name)}].push_back(&event);

    std::string out;
    out += "# OpenMinerZ modding API reference\n\n";
    out += std::format("API version {}. {} functions in {} libraries, {} events, {} constants.\n\n",
                       modding::api_version, registry.functions().size(), by_library.size(),
                       registry.events().size(), registry.constants().size());
    out += "> Generated by `openminerz_mod_sdk reference` from the registry the game itself builds\n"
           "> (every area registered on stand-in backends). Do not edit: the CTest\n"
           "> `openminerz_mod_sdk_reference` fails when this file no longer matches the code.\n"
           "> Editor type definitions: `tools/ModSdk/types/openminerz.d.luau`; machine-readable:\n"
           "> `tools/ModSdk/types/openminerz-api.json`.\n\n";
    out += "How to read an entry:\n\n";
    out += "- **Runs on** is the VM the function exists on. Calling it on the other side raises\n"
           "  \"... is only available on the server\" (or client).\n";
    out += "- **Needs** is the capability the calling mod must have. Client mods get `presentation`,\n"
           "  `local_storage` and `query`. Server mods get `gameplay`, `network`, `world_storage`, `query`\n"
           "  on the server VM and `presentation`, `local_storage`, `network`, `query` on the client VM.\n";
    out += "- **Usable by** spells that out: which kinds of mod can call it, and from which VM.\n";
    out += "- `vector` is Luau's native vector (`vector.create(x, y, z)`); a vector parameter also takes\n"
           "  `{x = , y = , z = }`. `Handle` is an engine object (`.kind`, `.id`). `table` is a table with\n"
           "  string keys, `array` a list. A `?` marks an optional parameter.\n\n";
    out += "See [authoring.md](authoring.md) for sides and capabilities, and the guides for worked examples.\n\n";

    out += "## Libraries\n\n| Library | Area | Functions | For |\n|---|---|---|---|\n";
    for (const auto &[library, functions] : by_library)
    {
        const LibraryInfo *info = library_info(library);
        out += std::format("| [`{}`](#{}) | {} | {} | {} |\n", library, library, info ? info->area : "",
                           functions.size(), info ? info->summary : "");
    }
    out += "\nEvents: ";
    {
        bool first = true;
        for (const auto &[group, events] : events_by_group)
        {
            out += std::format("{}[`{}`](#events-{})", first ? "" : ", ", group, group);
            first = false;
        }
    }
    out += ".\n\n";

    for (const auto &[library, functions] : by_library)
    {
        const LibraryInfo *info = library_info(library);
        out += std::format("## {}\n\n", library);
        if (info != nullptr)
            out += std::format("{} ({} area).\n\n", info->summary, info->area);
        if (reserved_global(library))
            out += "**Not reachable from scripts:** the name is a Luau global, so the runtime skips it.\n\n";
        for (const FunctionSpec *spec : functions)
        {
            out += std::format("### {}\n\n", spec->name);
            out += std::format("```luau\n{}\n```\n\n", signature(*spec));
            if (!spec->doc.empty())
                out += spec->doc + "\n\n";
            out += std::format("**Runs on:** {} · **Needs:** {} · **Usable by:** {}\n\n", side_text(spec->sides),
                               spec->requires_caps == 0U ? std::string{"nothing"} : capability_names(spec->requires_caps),
                               availability_text(*spec));
            if (!spec->params.empty() || spec->variadic)
            {
                out += "| Parameter | Type | Description |\n|---|---|---|\n";
                for (const ParamSpec &param : spec->params)
                    out += std::format("| `{}` | `{}`{} | {} |\n", param.name, doc_type(param.type),
                                       param.optional ? " (optional)" : "", escape_cell(param.doc));
                if (spec->variadic)
                    out += "| `...` | `any` | more values |\n";
                out += "\n";
            }
            if (spec->returns != ValueType::nil)
                out += std::format("**Returns** `{}`{}{}\n\n", doc_type(spec->returns),
                                   spec->returns_doc.empty() ? "" : ": ", spec->returns_doc);
        }
    }

    out += "## Events\n\n";
    out += "Subscribe with `events.on(name, function(event) ... end)`; `event` is a table with the\n"
           "fields listed. A handler of a **cancellable** event returns `false` to cancel it. Your own\n"
           "events are named `\"<your mod id>:<name>\"` and fired with `events.emit`.\n\n";
    for (const auto &[group, events] : events_by_group)
    {
        out += std::format("<a id=\"events-{}\"></a>\n\n### Events: {}\n\n", group, group);
        for (const EventSpec *event : events)
        {
            out += std::format("#### {}\n\n", event->name);
            if (!event->doc.empty())
                out += event->doc + "\n\n";
            out += std::format("**Fires on:** {}{}\n\n", side_text(event->sides),
                               event->cancellable ? " · **Cancellable** (return `false`)" : "");
            if (!event->payload.empty())
            {
                out += "| Field | Type | Description |\n|---|---|---|\n";
                for (const ParamSpec &field : event->payload)
                    out += std::format("| `{}` | `{}`{} | {} |\n", field.name, doc_type(field.type),
                                       field.optional ? " (may be nil)" : "", escape_cell(field.doc));
                out += "\n";
            }
        }
    }

    // Retail names, for fallbacks and every function that takes a block or an
    // item, with the game versions that have each (protocol=both needs both).
    out += "## Retail names\n\n<a id=\"retail-names\"></a>\n\n";
    out += "Scripts name retail blocks and items like this; mod content is `\"<mod id>:<name>\"`.\n"
           "*Both* means 1.6.3.8 (Classic) and 1.9.9.8 (Modern) have it; a mod with `protocol=both`\n"
           "should use only those as fallbacks.\n\n";
    out += "### Blocks\n\n| Id | Name | Versions |\n|---|---|---|\n";
    for (int id = 0; id <= 93; ++id)
    {
        const std::string_view name = modding::gameplay::retail_block_name(static_cast<std::uint8_t>(id));
        if (name.empty())
            continue;
        out += std::format("| {} | `{}` | {} |\n", id, id == 0 ? std::string_view{"air"} : name,
                           id <= 46 ? "both" : "Modern");
    }
    out += "\n### Items\n\n| Id | Name | Versions |\n|---|---|---|\n";
    for (int id = 1; id < static_cast<int>(InventoryItemId::item_count); ++id)
    {
        const std::string_view name = modding::gameplay::retail_item_name(static_cast<std::uint16_t>(id));
        const bool classic = item_registered(static_cast<InventoryItemId>(id), InventoryProfile::classic);
        const bool modern = item_registered(static_cast<InventoryItemId>(id), InventoryProfile::modern);
        if (name.empty() || (!classic && !modern))
            continue;
        out += std::format("| {} | `{}` | {} |\n", id, name, classic && modern ? "both" : classic ? "Classic" : "Modern");
    }
    out += "\n";

    if (!registry.constants().empty())
    {
        out += "## Constants\n\n| Name | Value | Runs on | Description |\n|---|---|---|---|\n";
        for (const auto &[name, constant] : registry.constants())
        {
            std::string value;
            std::string error;
            if (!modding::serialize_value(constant.value, value, error))
                value = "?";
            out += std::format("| `{}` | `{}` | {} | {} |\n", name, escape_cell(value), side_text(constant.sides),
                               escape_cell(constant.doc));
        }
        out += "\n";
    }
    while (out.ends_with("\n\n"))
        out.pop_back();
    return out;
}

void json_params(std::string &out, const std::vector<ParamSpec> &params, std::string_view indent)
{
    out += "[";
    for (std::size_t index = 0U; index < params.size(); ++index)
    {
        const ParamSpec &param = params[index];
        out += std::format("{}\n{}  {{\"name\": {}, \"type\": {}, \"optional\": {}, \"doc\": {}}}",
                           index == 0U ? "" : ",", indent, json_string(param.name),
                           json_string(doc_type(param.type)), param.optional ? "true" : "false",
                           json_string(param.doc));
    }
    out += params.empty() ? "]" : std::format("\n{}]", indent);
}

[[nodiscard]] std::string json_sides(SideMask sides)
{
    if (sides == modding::side_both)
        return "[\"client\", \"server\"]";
    return sides == modding::side_server ? "[\"server\"]" : "[\"client\"]";
}

[[nodiscard]] std::string json_capabilities(CapabilityMask mask)
{
    std::string out = "[";
    bool first = true;
    const std::string names = capability_names(mask);
    std::size_t start{};
    while (mask != 0U && start < names.size())
    {
        const std::size_t comma = names.find(", ", start);
        out += std::format("{}{}", first ? "" : ", ",
                           json_string(names.substr(start, comma == std::string::npos ? std::string::npos
                                                                                       : comma - start)));
        first = false;
        if (comma == std::string::npos)
            break;
        start = comma + 2U;
    }
    return out + "]";
}

[[nodiscard]] std::string generate_json(const ApiRegistry &registry)
{
    std::string out = "{\n";
    out += std::format("  \"api_version\": {},\n", modding::api_version);
    out += "  \"generator\": \"openminerz_mod_sdk reference\",\n";
    out += "  \"functions\": [";
    bool first = true;
    for (const auto &[name, function] : registry.functions())
    {
        const FunctionSpec &spec = function.spec;
        const Availability usable = availability(spec);
        out += std::format("{}\n    {{\n      \"name\": {},\n      \"library\": {},\n      \"doc\": {},\n",
                           first ? "" : ",", json_string(name), json_string(library_of(name)), json_string(spec.doc));
        out += std::format("      \"sides\": {},\n      \"requires\": {},\n", json_sides(spec.sides),
                           json_capabilities(spec.requires_caps));
        out += "      \"params\": ";
        json_params(out, spec.params, "      ");
        out += std::format(",\n      \"variadic\": {},\n      \"returns\": {{\"type\": {}, \"doc\": {}}},\n",
                           spec.variadic ? "true" : "false", json_string(doc_type(spec.returns)),
                           json_string(spec.returns_doc));
        out += std::format("      \"usable_by\": {{\"client_mod\": {}, \"server_mod_server_vm\": {}, "
                           "\"server_mod_client_vm\": {}}}\n    }}",
                           usable.client_mod ? "true" : "false", usable.server_mod_server ? "true" : "false",
                           usable.server_mod_client ? "true" : "false");
        first = false;
    }
    out += "\n  ],\n  \"events\": [";
    first = true;
    for (const auto &[name, event] : registry.events())
    {
        out += std::format("{}\n    {{\n      \"name\": {},\n      \"doc\": {},\n      \"sides\": {},\n"
                           "      \"cancellable\": {},\n      \"payload\": ",
                           first ? "" : ",", json_string(name), json_string(event.doc), json_sides(event.sides),
                           event.cancellable ? "true" : "false");
        json_params(out, event.payload, "      ");
        out += "\n    }";
        first = false;
    }
    out += "\n  ],\n  \"constants\": [";
    first = true;
    for (const auto &[name, constant] : registry.constants())
    {
        std::string value;
        std::string error;
        if (!modding::serialize_value(constant.value, value, error))
            value = "null";
        out += std::format("{}\n    {{\"name\": {}, \"value\": {}, \"doc\": {}, \"sides\": {}}}", first ? "" : ",",
                           json_string(name), value, json_string(constant.doc), json_sides(constant.sides));
        first = false;
    }
    out += "\n  ]\n}\n";
    return out;
}

} // namespace

Availability availability(const FunctionSpec &spec) noexcept
{
    using modding::Side;
    using modding::capabilities_for;
    const auto allowed = [&](net::ModScope scope, Side side) {
        return modding::side_in(side, spec.sides) && modding::runs_on(scope, side) &&
               (spec.requires_caps & ~capabilities_for(scope, side)) == 0U;
    };
    return {allowed(net::ModScope::client, Side::client), allowed(net::ModScope::server, Side::server),
            allowed(net::ModScope::server, Side::client)};
}

std::string capability_names(CapabilityMask mask)
{
    namespace capability = modding::capability;
    constexpr std::array<std::pair<CapabilityMask, std::string_view>, 6U> names{{
        {capability::presentation, "presentation"},
        {capability::gameplay, "gameplay"},
        {capability::network, "network"},
        {capability::world_storage, "world_storage"},
        {capability::local_storage, "local_storage"},
        {capability::query, "query"},
    }};
    std::string text;
    for (const auto &[bit, name] : names)
        if ((mask & bit) != 0U)
            text += std::format("{}{}", text.empty() ? "" : ", ", name);
    return text;
}

std::string_view side_text(SideMask sides) noexcept
{
    if (sides == modding::side_both)
        return "server and client";
    return sides == modding::side_server ? "server" : "client";
}

std::string_view library_of(std::string_view name) noexcept
{
    return name.substr(0U, name.find('.'));
}

bool reserved_global(std::string_view name) noexcept
{
    // Luau's own globals (the runtime keeps them; a library may not replace
    // one) plus the two the runtime defines per file.
    constexpr std::string_view globals[]{
        "_G",      "_VERSION", "assert",   "bit32",        "buffer",   "collectgarbage", "coroutine",
        "error",   "gcinfo",   "getmetatable", "ipairs",   "math",     "newproxy",       "next",
        "pairs",   "pcall",    "print",    "rawequal",     "rawget",   "rawlen",         "rawset",
        "require", "select",   "setmetatable", "string",   "table",    "tonumber",       "tostring",
        "type",    "typeof",   "unpack",   "utf8",         "vector",   "xpcall"};
    return std::find(std::begin(globals), std::end(globals), name) != std::end(globals);
}

GeneratedFiles generate_reference(const ApiRegistry &registry)
{
    return {generate_markdown(registry) + "\n", generate_luau(registry) + "\n", generate_json(registry)};
}

} // namespace openminerz::mod_sdk
