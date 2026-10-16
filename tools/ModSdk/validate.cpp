// `openminerz_mod_sdk validate` (validate.hpp).

#include "validate.hpp"

#include "reference.hpp"
#include "sdk_session.hpp"
#include "world_tools.hpp" // mod-world
#include "ai_tools.hpp"    // mod-ai

#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/modding/graphics.hpp"
#include "openminerz/modding/package.hpp"
#include "openminerz/modding/runtime.hpp"
#include "openminerz/modding/ui.hpp"
#include "openminerz/net/mod_download.hpp"

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Lexer.h"
#include "Luau/ParseOptions.h"
#include "Luau/Parser.h"
#include "luacode.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace openminerz::mod_sdk
{

using modding::Side;
using modding::SideMask;

std::size_t ValidationResult::count(Diagnostic::Level level) const noexcept
{
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                  [level](const Diagnostic &d) { return d.level == level; }));
}

namespace
{

constexpr std::uint64_t maximum_script_bytes = 4U * 1024U * 1024U; // package.cpp

std::string lowercase(std::string_view text)
{
    std::string result{text};
    for (char &character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

bool is_script(std::string_view path)
{
    return path.ends_with(".luau") || path.ends_with(".lua");
}

std::optional<std::string> read_file(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// "a/./b/../c" -> "a/c"; nullopt when it climbs out of the mod.
std::optional<std::string> normalize(std::string_view path)
{
    std::vector<std::string> parts;
    std::size_t start{};
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                                         : slash - start);
        if (part == "..")
        {
            if (parts.empty())
                return std::nullopt;
            parts.pop_back();
        }
        else if (!part.empty() && part != ".")
            parts.emplace_back(lowercase(part));
        if (slash == std::string_view::npos)
            break;
        start = slash + 1U;
    }
    if (parts.empty())
        return std::nullopt;
    std::string result;
    for (const std::string &part : parts)
        result += (result.empty() ? "" : "/") + part;
    return result;
}

std::size_t edit_distance(std::string_view a, std::string_view b)
{
    std::vector<std::size_t> row(b.size() + 1U);
    for (std::size_t j = 0U; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1U; i <= a.size(); ++i)
    {
        std::size_t previous = row[0];
        row[0] = i;
        for (std::size_t j = 1U; j <= b.size(); ++j)
        {
            const std::size_t current = row[j];
            row[j] = std::min({row[j] + 1U, row[j - 1U] + 1U, previous + (a[i - 1U] == b[j - 1U] ? 0U : 1U)});
            previous = current;
        }
    }
    return row[b.size()];
}

// " (did you mean world.get_block?)" or "".
template <typename Names>
std::string suggestion(std::string_view wanted, const Names &names)
{
    std::string best;
    std::size_t best_distance = 4U;
    for (const auto &entry : names)
    {
        const std::string_view name = entry.first;
        const std::size_t distance = edit_distance(wanted, name);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = std::string{name};
        }
    }
    return best.empty() ? std::string{} : std::format(" (did you mean {}?)", best);
}

struct Script final
{
    std::string path; // normalized, mod-relative
    std::string source;
    SideMask sides{};  // where it can run (0: never, for a client mod's server/)
    bool loaded_at_start{}; // directly inside shared/, client/ or server/
};

struct RequireEdge final
{
    std::string from;
    std::string to;
    int line{};
    bool top_level{};
};

class Checker;

struct Context final
{
    const modding::ApiRegistry &registry;
    const net::LoadedMod &mod;
    std::map<std::string, Script> scripts; // by normalized path
    std::set<std::string> files;           // every normalized file path
    std::set<std::string, std::less<>> libraries;
    std::vector<std::string> dependencies; // lowercase ids
    std::vector<RequireEdge> require_edges;
    std::vector<Diagnostic> &out;

    void report(Diagnostic::Level level, const std::string &file, const Luau::Location &location,
                std::string message)
    {
        out.push_back({level, file, static_cast<int>(location.begin.line) + 1,
                       static_cast<int>(location.begin.column) + 1, std::move(message)});
    }

    [[nodiscard]] modding::CapabilityMask granted(Side side) const noexcept
    {
        return modding::capabilities_for(mod.descriptor.scope, side);
    }
    [[nodiscard]] bool client_mod() const noexcept
    {
        return mod.descriptor.scope == net::ModScope::client;
    }
};

std::string side_list(SideMask sides)
{
    return std::string{side_text(sides)};
}

// Walks one script: API names, sides, capabilities, events, require, writes
// to libraries, sandboxed globals and asset paths.
class Checker final : public Luau::AstVisitor
{
  public:
    Checker(Context &context, const Script &script) : context_{context}, script_{script} {}

    bool visit(Luau::AstExprFunction *node) override
    {
        ++function_depth_;
        node->body->visit(this);
        --function_depth_;
        return false;
    }

    bool visit(Luau::AstStatIf *node) override
    {
        node->condition->visit(this);
        ++guards_;
        node->thenbody->visit(this);
        if (node->elsebody != nullptr)
            node->elsebody->visit(this);
        --guards_;
        return false;
    }

    bool visit(Luau::AstStatAssign *node) override
    {
        for (Luau::AstExpr *target : node->vars)
            if (!assignment_target(target))
                target->visit(this);
        for (Luau::AstExpr *value : node->values)
            value->visit(this);
        return false;
    }

    bool visit(Luau::AstStatCompoundAssign *node) override
    {
        if (!assignment_target(node->var))
            node->var->visit(this);
        node->value->visit(this);
        return false;
    }

    bool visit(Luau::AstStatFunction *node) override
    {
        if (!assignment_target(node->name))
            node->name->visit(this);
        node->func->visit(this);
        return false;
    }

    bool visit(Luau::AstExprGlobal *node) override
    {
        static constexpr std::string_view removed[]{"os",     "io",       "debug",  "getfenv",
                                                    "setfenv", "loadstring", "dofile", "loadfile"};
        const std::string_view name = node->name.value;
        if (std::find(std::begin(removed), std::end(removed), name) != std::end(removed))
            context_.report(Diagnostic::Level::error, script_.path, node->location,
                            std::format("'{}' is not available to mods (the sandbox removes it)", name));
        return false;
    }

    bool visit(Luau::AstExprIndexName *node) override
    {
        std::string path;
        Luau::AstExprGlobal *root{};
        if (!chain(node, path, root))
            return true;
        check_name(path, node->location, nullptr);
        return false;
    }

    bool visit(Luau::AstExprCall *node) override
    {
        if (auto *global = node->func->as<Luau::AstExprGlobal>();
            global != nullptr && std::string_view{global->name.value} == "require")
            check_require(node);
        std::string path;
        Luau::AstExprGlobal *root{};
        if (auto *index = node->func->as<Luau::AstExprIndexName>(); index != nullptr && chain(index, path, root))
        {
            if (check_name(path, node->location, node))
                check_call(path, node);
            for (Luau::AstExpr *argument : node->args)
                argument->visit(this);
            return false;
        }
        return true;
    }

    bool visit(Luau::AstExprConstantString *node) override
    {
        if (node->quoteStyle != Luau::AstExprConstantString::Unquoted)
            check_asset(std::string_view{node->value.data, node->value.size}, node->location);
        return false;
    }

  private:
    // world.x.y -> "world.x.y" when the chain starts at a library global.
    bool chain(Luau::AstExprIndexName *node, std::string &path, Luau::AstExprGlobal *&root)
    {
        std::vector<std::string_view> parts;
        Luau::AstExpr *at = node;
        while (auto *index = at->as<Luau::AstExprIndexName>())
        {
            if (index->op != '.')
                return false;
            parts.emplace_back(index->index.value);
            at = index->expr;
        }
        root = at->as<Luau::AstExprGlobal>();
        if (root == nullptr || !context_.libraries.contains(std::string_view{root->name.value}))
            return false;
        path = root->name.value;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part)
            path += "." + std::string{*part};
        return true;
    }

    // True when the target was a library (and reported).
    bool assignment_target(Luau::AstExpr *target)
    {
        if (auto *global = target->as<Luau::AstExprGlobal>();
            global != nullptr && context_.libraries.contains(std::string_view{global->name.value}))
        {
            context_.report(Diagnostic::Level::error, script_.path, target->location,
                            std::format("assigning to '{}' hides the {} library in this file", global->name.value,
                                        global->name.value));
            return true;
        }
        std::string path;
        Luau::AstExprGlobal *root{};
        if (auto *index = target->as<Luau::AstExprIndexName>(); index != nullptr && chain(index, path, root))
        {
            context_.report(Diagnostic::Level::error, script_.path, target->location,
                            std::format("library tables are read-only: cannot assign {}", path));
            return true;
        }
        return false;
    }

    [[nodiscard]] bool unconditional() const noexcept
    {
        return function_depth_ == 0 && guards_ == 0;
    }

    // Reports an unknown name; true when `path` is a registered function.
    bool check_name(const std::string &path, const Luau::Location &location, Luau::AstExprCall *call)
    {
        const modding::ApiRegistry &registry = context_.registry;
        if (registry.find_function(path) != nullptr)
            return true;
        if (registry.constants().contains(path))
            return false;
        const std::string prefix = path + ".";
        const auto next = registry.functions().lower_bound(prefix);
        if (next != registry.functions().end() && next->first.starts_with(prefix))
        {
            if (call != nullptr)
                context_.report(Diagnostic::Level::error, script_.path, location,
                                std::format("{} is a library table, not a function", path));
            return false;
        }
        std::map<std::string, int> same_library;
        const std::string library = std::string{library_of(path)} + ".";
        for (auto it = registry.functions().lower_bound(library);
             it != registry.functions().end() && it->first.starts_with(library); ++it)
            same_library.emplace(it->first, 0);
        context_.report(Diagnostic::Level::error, script_.path, location,
                        std::format("unknown API '{}'{}", path, suggestion(path, same_library)));
        return false;
    }

    void check_call(const std::string &path, Luau::AstExprCall *call)
    {
        const modding::FunctionSpec &spec = context_.registry.find_function(path)->spec;
        // Which of the sides this file runs on refuse the call, and why.
        std::vector<std::string> refusals;
        SideMask refused_sides{};
        for (const Side side : {Side::server, Side::client})
        {
            if (!modding::side_in(side, script_.sides))
                continue;
            std::string why;
            if (!modding::side_in(side, spec.sides))
                why = std::format("{} is only available on the {}", path, side_list(spec.sides));
            else if (const modding::CapabilityMask missing = spec.requires_caps & ~context_.granted(side);
                     missing != 0U)
                why = context_.client_mod()
                          ? std::format("{} needs the {} capability, which client mods do not have", path,
                                        capability_names(missing))
                          : std::format("{} needs the {} capability, which a server mod does not have on the {}",
                                        path, capability_names(missing), modding::side_name(side));
            if (!why.empty())
            {
                refusals.push_back(std::move(why));
                refused_sides |= static_cast<SideMask>(side);
            }
        }
        if (refusals.empty())
        {
            check_event_call(path, call);
            return;
        }
        if (refused_sides == script_.sides)
        {
            context_.report(Diagnostic::Level::error, script_.path, call->location,
                            refusals.front() + (script_.sides == modding::side_both
                                                    ? std::string{}
                                                    : std::format(" (this file runs on the {})",
                                                                  side_list(script_.sides))));
            return;
        }
        // A shared file: fine inside a function or an `if`, fatal when it runs
        // unconditionally while the other side loads the file.
        if (unconditional())
            context_.report(Diagnostic::Level::error, script_.path, call->location,
                            std::format("{}: shared/ files load on both sides, so this fails when the {} loads it "
                                        "(move it to {}/ or guard it with `if mod.side() == \"{}\" then`)",
                                        refusals.front(),
                                        modding::side_name(refused_sides == modding::side_server ? Side::server
                                                                                                 : Side::client),
                                        refused_sides == modding::side_server ? "client" : "server",
                                        refused_sides == modding::side_server ? "client" : "server"));
        check_event_call(path, call);
    }

    void check_event_call(const std::string &path, Luau::AstExprCall *call)
    {
        if (path != "events.on" && path != "events.once" && path != "events.off" && path != "events.emit")
            return;
        if (call->args.size == 0U)
            return;
        auto *literal = call->args.data[0]->as<Luau::AstExprConstantString>();
        if (literal == nullptr)
            return;
        const std::string name{literal->value.data, literal->value.size};
        const std::size_t colon = name.find(':');
        if (path == "events.emit")
        {
            if (colon == std::string::npos || lowercase(name.substr(0U, colon)) != lowercase(context_.mod.descriptor.id))
                context_.report(Diagnostic::Level::error, script_.path, literal->location,
                                std::format("a mod emits only its own events, named \"{}:<name>\"",
                                            context_.mod.descriptor.id));
            return;
        }
        if (colon != std::string::npos && colon > 0U && colon + 1U < name.size())
            return; // a mod's own event: any name goes
        const modding::EventSpec *event = context_.registry.find_event(name);
        if (event == nullptr)
        {
            context_.report(Diagnostic::Level::error, script_.path, literal->location,
                            std::format("unknown event '{}'{}", name, suggestion(name, context_.registry.events())));
            return;
        }
        const SideMask missing = static_cast<SideMask>(script_.sides & ~event->sides);
        if (missing == 0U)
            return;
        const std::string message = std::format("event '{}' only fires on the {}", name, side_list(event->sides));
        if (missing == script_.sides)
            context_.report(Diagnostic::Level::error, script_.path, literal->location,
                            message + std::format(" (this file runs on the {})", side_list(script_.sides)));
        else if (unconditional())
            context_.report(Diagnostic::Level::error, script_.path, literal->location,
                            message + ": subscribing at the top of a shared/ file fails when the other side "
                                      "loads it (subscribe from server/ or client/, or guard it with mod.side())");
    }

    void check_require(Luau::AstExprCall *call)
    {
        if (call->args.size != 1U)
            return;
        auto *literal = call->args.data[0]->as<Luau::AstExprConstantString>();
        if (literal == nullptr)
        {
            context_.report(Diagnostic::Level::warning, script_.path, call->location,
                            "require() with a computed path cannot be checked");
            return;
        }
        const std::string request{literal->value.data, literal->value.size};
        std::string relative;
        if (request.starts_with('@'))
        {
            const std::size_t slash = request.find('/');
            const std::string name = lowercase(request.substr(1U, slash == std::string::npos ? slash : slash - 1U));
            relative = slash == std::string::npos ? std::string{"init"} : request.substr(slash + 1U);
            if (name != "self" && name != lowercase(context_.mod.descriptor.id))
            {
                if (std::find(context_.dependencies.begin(), context_.dependencies.end(), name) ==
                    context_.dependencies.end())
                    context_.report(Diagnostic::Level::error, script_.path, literal->location,
                                    std::format("'{}' is not a declared dependency (add it to depends= in "
                                                "openminerz.mod)",
                                                name));
                return; // another mod's files are not checked here
            }
        }
        else if (request.starts_with("./") || request.starts_with("../"))
        {
            const std::size_t slash = script_.path.rfind('/');
            relative = (slash == std::string::npos ? std::string{} : script_.path.substr(0U, slash + 1U)) + request;
        }
        else
            relative = request;
        const std::optional<std::string> normalized = normalize(relative);
        if (!normalized)
        {
            context_.report(Diagnostic::Level::error, script_.path, literal->location,
                            std::format("'{}' is not a path inside the mod", request));
            return;
        }
        for (const Side side : {Side::server, Side::client})
        {
            const std::string_view forbidden = side == Side::client ? "server/" : "client/";
            if (modding::side_in(side, script_.sides) && normalized->starts_with(forbidden))
                context_.report(Diagnostic::Level::error, script_.path, literal->location,
                                std::format("'{}' is {} code and cannot be required on the {}", request,
                                            side == Side::client ? "server" : "client", modding::side_name(side)));
        }
        std::string found;
        if (is_script(*normalized) && context_.scripts.contains(*normalized))
            found = *normalized;
        for (const std::string &candidate : {*normalized + ".luau", *normalized + ".lua", *normalized + "/init.luau",
                                             *normalized + "/init.lua"})
            if (found.empty() && context_.scripts.contains(candidate))
                found = candidate;
        if (found.empty())
        {
            const bool outside = !normalized->starts_with("shared/") && !normalized->starts_with("client/") &&
                                 !normalized->starts_with("server/");
            context_.report(Diagnostic::Level::error, script_.path, literal->location,
                            std::format("no module '{}' in this mod{}", request,
                                        outside ? " (modules live under shared/, client/ or server/; paths "
                                                  "without ./ start at the mod's root)"
                                                : ""));
            return;
        }
        context_.require_edges.push_back({script_.path, found, static_cast<int>(literal->location.begin.line) + 1,
                                     function_depth_ == 0});
    }

    void check_asset(std::string_view text, const Luau::Location &location)
    {
        static constexpr std::string_view graphics_folders[]{"textures/", "models/", "sounds/",
                                                             "music/",    "luts/",  "animations/"};
        if (text.size() < 5U || text.size() > 240U ||
            !std::all_of(text.begin(), text.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '/' || c == '.' || c == '_' ||
                       c == '-';
            }))
            return;
        if (text.starts_with("assets/") || text.starts_with("lang/"))
        {
            if (text.find('.') == std::string_view::npos)
                return;
            const std::optional<std::string> normalized = normalize(text);
            if (normalized && !context_.files.contains(*normalized))
                context_.report(Diagnostic::Level::warning, script_.path, location,
                                std::format("'{}' is not a file in this mod", text));
            return;
        }
        const bool graphics_name = std::any_of(std::begin(graphics_folders), std::end(graphics_folders),
                                               [&](std::string_view folder) { return text.starts_with(folder); });
        if (!graphics_name)
            return;
        static constexpr std::string_view extensions[]{"", ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gltf",
                                                       ".glb", ".wav", ".ogg"};
        const std::optional<std::string> normalized = normalize("assets/" + std::string{text});
        if (!normalized)
            return;
        if (std::none_of(std::begin(extensions), std::end(extensions), [&](std::string_view extension) {
                return context_.files.contains(*normalized + std::string{extension});
            }))
            context_.report(Diagnostic::Level::warning, script_.path, location,
                            std::format("asset '{}' is not in this mod's assets/ folder (fine if a mod loaded "
                                        "before this one provides it)",
                                        text));
    }

    Context &context_;
    const Script &script_;
    int function_depth_{};
    int guards_{};
};

void check_script(Context &context, const Script &script)
{
    Luau::Allocator allocator;
    Luau::AstNameTable names{allocator};
    Luau::ParseOptions options;
    const Luau::ParseResult parsed =
        Luau::Parser::parse(script.source.data(), script.source.size(), names, allocator, options);
    for (const Luau::ParseError &error : parsed.errors)
        context.report(Diagnostic::Level::error, script.path, error.getLocation(), error.getMessage());
    if (!parsed.errors.empty())
        return;

    // The game's own compiler and options (runtime.cpp: compile).
    lua_CompileOptions compile{};
    compile.optimizationLevel = 1;
    compile.debugLevel = 1;
    std::size_t size{};
    char *bytecode = luau_compile(script.source.data(), script.source.size(), &compile, &size);
    if (bytecode != nullptr && size > 0U && bytecode[0] == '\0')
    {
        // ":<line>: message"
        const std::string message{bytecode + 1, size - 1U};
        int line{};
        std::string text = message;
        if (message.starts_with(':'))
        {
            const std::size_t colon = message.find(':', 1U);
            if (colon != std::string::npos)
            {
                std::from_chars(message.data() + 1, message.data() + colon, line);
                text = message.substr(std::min(message.size(), colon + 2U));
            }
        }
        context.out.push_back({Diagnostic::Level::error, script.path, line, 0, "compile error: " + text});
    }
    std::free(bytecode);
    if (script.sides == 0U)
        return;
    Checker checker{context, script};
    parsed.root->visit(&checker);
}

void check_require_cycles(Context &context)
{
    std::map<std::string, std::vector<const RequireEdge *>> graph;
    for (const RequireEdge &edge : context.require_edges)
        if (edge.top_level)
            graph[edge.from].push_back(&edge);
    std::map<std::string, int> state; // 1 visiting, 2 done
    std::vector<const RequireEdge *> stack;
    std::set<std::string> reported;
    const std::function<void(const std::string &)> walk = [&](const std::string &node) {
        state[node] = 1;
        for (const RequireEdge *edge : graph[node])
        {
            stack.push_back(edge);
            if (state[edge->to] == 1)
            {
                std::string cycle;
                bool inside = false;
                for (const RequireEdge *step : stack)
                {
                    inside = inside || step->from == edge->to;
                    if (inside)
                        cycle += step->from + " -> ";
                }
                cycle += edge->to;
                if (reported.insert(edge->from).second)
                    context.out.push_back({Diagnostic::Level::error, edge->from, edge->line, 0,
                                           "require cycle: " + cycle + " (a module cannot require one that is "
                                                                       "still loading)"});
            }
            else if (state[edge->to] == 0)
                walk(edge->to);
            stack.pop_back();
        }
        state[node] = 2;
    };
    for (const auto &[node, edges] : graph)
        if (state[node] == 0)
            walk(node);
}

// (Polish) What the game skips in a mod folder (net::is_ignored_mod_entry),
// outermost entries only, mod-relative: ".git/", ".luaurc", "assets/Thumbs.db".
std::vector<std::string> ignored_entries(const std::filesystem::path &root)
{
    std::vector<std::string> found;
    std::error_code fs_error;
    for (std::filesystem::recursive_directory_iterator it{root, fs_error}, end; !fs_error && it != end;
         it.increment(fs_error))
    {
        if (!net::is_ignored_mod_entry(it->path().filename()))
            continue;
        it.disable_recursion_pending();
        const std::u8string relative = it->path().lexically_relative(root).generic_u8string();
        std::string name{relative.begin(), relative.end()};
        std::error_code kind_error;
        if (it->is_directory(kind_error))
            name += '/';
        found.push_back(std::move(name));
    }
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

ValidationResult validate_mod(const std::filesystem::path &folder, const modding::ApiRegistry &registry)
{
    ValidationResult result;
    std::vector<Diagnostic> &out = result.diagnostics;
    const auto mod_error = [&](std::string file, std::string message) {
        out.push_back({Diagnostic::Level::error, std::move(file), 0, 0, std::move(message)});
    };
    const auto mod_warning = [&](std::string file, std::string message) {
        out.push_back({Diagnostic::Level::warning, std::move(file), 0, 0, std::move(message)});
    };

    std::string error;
    if (!load_mod_folder(folder, result.mod, error))
    {
        mod_error("openminerz.mod", error);
        return result;
    }
    result.loaded = true;
    const net::LoadedMod &mod = result.mod;
    result.digest = net::digest_hex(mod.descriptor.content_digest);

    modding::PackageInfo info;
    if (!modding::parse_package_info(mod, info, error))
        mod_error("openminerz.mod", error);
    else if (info.api > modding::api_version)
        mod_error("openminerz.mod", std::format("api={} is newer than this game's modding API ({})", info.api,
                                                modding::api_version));

    // Files: the allowlist, every problem at once (read_package stops at the
    // first), sizes and names.
    std::vector<net::ModContentFile> files;
    if (!net::list_mod_content(mod.root, files, error))
    {
        mod_error("", error);
        return result;
    }
    // (Polish) Editor and repository files are fine to keep in the folder;
    // say once that the game leaves them out.
    if (const std::vector<std::string> ignored = ignored_entries(mod.root); !ignored.empty())
    {
        std::string names;
        for (std::size_t index{}; index < ignored.size() && index < 6U; ++index)
            names += (index == 0U ? "" : ", ") + ignored[index];
        if (ignored.size() > 6U)
            names += std::format(" and {} more", ignored.size() - 6U);
        out.push_back({Diagnostic::Level::info, "", 0, 0,
                       "not part of the mod (never loaded, hashed, downloaded or packed): " + names});
    }
    Context context{registry, mod, {}, {}, {}, {}, {}, out};
    result.files = files.size();
    bool any_assets{};
    for (const net::ModContentFile &file : files)
    {
        result.bytes += file.size;
        context.files.insert(file.path);
        if (!modding::is_allowed_package_file(file.path))
        {
            mod_error(file.path, "not a file type mods may contain (scripts, text/data, images, models, audio "
                                 "and fonts only; the game refuses the whole mod)");
            continue;
        }
        any_assets = any_assets || file.path.starts_with("assets/");
        const bool code_folder =
            file.path.starts_with("shared/") || file.path.starts_with("client/") || file.path.starts_with("server/");
        if (is_script(file.path) && !code_folder)
            mod_warning(file.path, "scripts outside shared/, client/ and server/ are never loaded or required");
        if (!is_script(file.path) || !code_folder)
            continue;
        if (file.size > maximum_script_bytes)
        {
            mod_error(file.path, "scripts may be at most 4 MiB");
            continue;
        }
        const std::optional<std::string> text = read_file(file.source);
        if (!text)
        {
            mod_error(file.path, "cannot be read");
            continue;
        }
        Script script;
        script.path = file.path;
        script.source = *text;
        const std::size_t first = file.path.find('/');
        script.loaded_at_start = file.path.find('/', first + 1U) == std::string::npos;
        const std::string_view folder_name = std::string_view{file.path}.substr(0U, first);
        if (folder_name == "shared")
            script.sides = context.client_mod() ? modding::side_client : modding::side_both;
        else if (folder_name == "client")
            script.sides = modding::side_client;
        else
            script.sides = context.client_mod() ? SideMask{} : modding::side_server;
        context.scripts.emplace(file.path, std::move(script));
    }
    result.scripts = context.scripts.size();
    if (std::string package_error; out.empty())
    {
        modding::ModSource source;
        if (!modding::read_package(mod, source, package_error))
            mod_error("", "the game refuses the package: " + package_error);
    }

    // The client-visible part a joining player downloads.
    net::ModDigest client_digest{};
    if (net::client_visible_digest(mod.root, client_digest, result.client_bytes, error))
        result.client_digest = net::digest_hex(client_digest);
    if (!context.client_mod() && result.client_bytes > net::default_maximum_mod_download_bytes)
        mod_error("", std::format("the client-visible part is {:.1f} MiB; players joining a host download at most "
                                  "64 MiB per mod",
                                  static_cast<double>(result.client_bytes) / (1024.0 * 1024.0)));

    // Layout.
    const bool has_server_folder = std::any_of(context.files.begin(), context.files.end(),
                                               [](const std::string &path) { return path.starts_with("server/"); });
    if (context.client_mod() && has_server_folder)
        mod_warning("server/", "a client mod never runs server/ scripts (use scope=server for gameplay)");
    if (context.scripts.empty() && !any_assets)
        mod_warning("", "the mod has no scripts and no assets/: it does nothing");
    for (const modding::PackageDependency &dependency : info.depends)
        context.dependencies.push_back(lowercase(dependency.id));

    // Dependencies installed next to it (when the folder's siblings load).
    if (!info.depends.empty())
    {
        net::ModCatalog siblings;
        std::string sibling_error;
        if (net::load_mod_catalog(mod.root.parent_path(), siblings, sibling_error))
            for (const modding::PackageDependency &dependency : info.depends)
            {
                const auto found = std::find_if(siblings.mods.begin(), siblings.mods.end(), [&](const net::LoadedMod &m) {
                    return lowercase(m.descriptor.id) == lowercase(dependency.id);
                });
                if (found == siblings.mods.end())
                    mod_warning("openminerz.mod",
                                std::format("depends on '{}', which is not installed next to this mod", dependency.id));
                else if (!modding::version_at_least(found->descriptor.version, dependency.minimum_version))
                    mod_error("openminerz.mod", std::format("needs {} {} or newer; {} is installed next to it",
                                                            dependency.id, dependency.minimum_version,
                                                            found->descriptor.version));
            }
    }

    // Asset names the graphics loader accepts (case-sensitive, from disk).
    std::error_code fs_error;
    const std::filesystem::path assets = mod.root / "assets";
    if (std::filesystem::is_directory(assets, fs_error))
        for (std::filesystem::recursive_directory_iterator it{assets, fs_error}, end; it != end;
             it.increment(fs_error))
        {
            if (!fs_error && net::is_ignored_mod_entry(it->path().filename()))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (fs_error || !it->is_regular_file(fs_error))
                continue;
            const std::string name = it->path().lexically_relative(assets).generic_string();
            std::string reason;
            if (!modding::graphics::valid_asset_name(name, &reason))
                mod_warning("assets/" + name, "the asset loader cannot name this file: " + reason);
        }

    // Lang files.
    for (const std::string &path : context.files)
    {
        if (!path.starts_with("lang/") || !path.ends_with(".lang"))
            continue;
        const auto source = std::find_if(files.begin(), files.end(),
                                         [&](const net::ModContentFile &file) { return file.path == path; });
        const std::optional<std::string> text = read_file(source->source);
        modding::ui::StringTable table;
        if (!text || !modding::ui::parse_lang_file(*text, table, error))
            mod_error(path, text ? error : "cannot be read");
    }

    // (Animation packs) Player clips: what the loader would refuse or ignore,
    // and the game's own clips, which a mod must never ship. Bone and clip
    // names only; `check-anim` also checks against the imported skeleton.
    for (const net::ModContentFile &file : files)
    {
        const std::filesystem::path path{file.path};
        if (!file.path.starts_with("assets/animations/") || path.parent_path() != "assets/animations" ||
            (path.extension() != ".glb" && path.extension() != ".gltf"))
            continue;
        const auto model = modding::graphics::load_model(file.source, &error,
                                                         modding::graphics::animation_file_limits());
        if (model == nullptr)
        {
            mod_error(file.path, error);
            continue;
        }
        const modding::graphics::AnimationCheck check =
            modding::graphics::check_animation(*model, path.stem().string());
        for (const modding::graphics::AnimationCheck::Message &message : check.messages)
            if (message.level != modding::graphics::AnimationCheck::Level::note)
                out.push_back({message.level == modding::graphics::AnimationCheck::Level::error
                                   ? Diagnostic::Level::error
                                   : Diagnostic::Level::warning,
                               file.path, 0, 0, message.text});
    }

    // Scripts.
    for (const auto &[name, function] : registry.functions())
        context.libraries.emplace(library_of(name));
    for (const auto &[name, constant] : registry.constants())
        context.libraries.emplace(library_of(name));
    for (const auto &[path, script] : context.scripts)
        check_script(context, script);
    check_require_cycles(context);
    // mod-world: a world generator is built the way the game builds it.
    check_world_generator(result);
    // mod-ai: creatures, brains and projectiles, loaded and checked together.
    check_creature_ai(result);

    std::stable_sort(out.begin(), out.end(), [](const Diagnostic &a, const Diagnostic &b) {
        if (a.file != b.file)
            return a.file < b.file;
        return a.line < b.line;
    });
    return result;
}

std::string format_diagnostics(const ValidationResult &result)
{
    std::string text;
    for (const Diagnostic &diagnostic : result.diagnostics)
    {
        std::string where = diagnostic.file.empty() ? std::string{"<mod>"} : diagnostic.file;
        if (diagnostic.line > 0)
            where += std::format(":{}", diagnostic.line);
        if (diagnostic.column > 0)
            where += std::format(":{}", diagnostic.column);
        const char *level = diagnostic.level == Diagnostic::Level::error     ? "error"
                            : diagnostic.level == Diagnostic::Level::warning ? "warning"
                                                                             : "info";
        text += std::format("{}: {}: {}\n", where, level, diagnostic.message);
    }
    return text;
}

} // namespace openminerz::mod_sdk
