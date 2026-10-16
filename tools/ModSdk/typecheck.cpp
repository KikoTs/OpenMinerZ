// Luau's own type checker over the generated definitions (typecheck.hpp).

#include "typecheck.hpp"

#include "Luau/Ast.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/ConfigResolver.h"
#include "Luau/Error.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"
#include "Luau/TypeArena.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace openminerz::mod_sdk
{

namespace
{

constexpr std::string_view definitions_name = "openminerz.d.luau";

// Scripts by their mod-relative path; require() strings resolve like the
// runtime resolves them (./x and ../x from the file, otherwise from the root).
class ModFiles final : public Luau::FileResolver
{
  public:
    explicit ModFiles(const std::map<std::string, std::string> &scripts) : scripts_{scripts} {}

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName &name) override
    {
        const auto found = scripts_.find(name);
        if (found == scripts_.end())
            return std::nullopt;
        return Luau::SourceCode{found->second, Luau::SourceCode::Module};
    }

    std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo *context, Luau::AstExpr *expr,
                                                  const Luau::TypeCheckLimits &) override
    {
        const auto *literal = expr->as<Luau::AstExprConstantString>();
        if (context == nullptr || literal == nullptr)
            return std::nullopt;
        std::string request{literal->value.data, literal->value.size};
        if (request.starts_with('@'))
            return std::nullopt; // another mod's files are not here
        std::string path;
        if (request.starts_with("./") || request.starts_with("../"))
        {
            const std::size_t slash = context->name.rfind('/');
            path = (slash == std::string::npos ? std::string{} : context->name.substr(0U, slash + 1U)) + request;
        }
        else
            path = request;
        std::vector<std::string> parts;
        std::size_t start{};
        while (start <= path.size())
        {
            const std::size_t slash = path.find('/', start);
            const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (part == "..")
            {
                if (parts.empty())
                    return std::nullopt;
                parts.pop_back();
            }
            else if (!part.empty() && part != ".")
            {
                std::string lowered = part;
                for (char &c : lowered)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                parts.push_back(std::move(lowered));
            }
            if (slash == std::string::npos)
                break;
            start = slash + 1U;
        }
        std::string normalized;
        for (const std::string &part : parts)
            normalized += (normalized.empty() ? "" : "/") + part;
        for (const std::string &candidate : {normalized, normalized + ".luau", normalized + ".lua",
                                             normalized + "/init.luau", normalized + "/init.lua"})
            if (scripts_.contains(candidate))
                return Luau::ModuleInfo{candidate};
        return std::nullopt;
    }

  private:
    const std::map<std::string, std::string> &scripts_;
};

void add_issue(std::vector<TypeIssue> &issues, std::string file, const Luau::Location &location, std::string message)
{
    issues.push_back({std::move(file), static_cast<int>(location.begin.line) + 1,
                      static_cast<int>(location.begin.column) + 1, std::move(message)});
}

// Builtins plus the definitions; problems loading them go to `issues`.
bool prepare(Luau::Frontend &frontend, std::string_view definitions, std::vector<TypeIssue> &issues)
{
    Luau::registerBuiltinGlobals(frontend, frontend.globals);
    Luau::unfreeze(frontend.globals.globalTypes);
    const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
        frontend.globals, frontend.globals.globalScope, definitions, "@openminerz", false, false);
    Luau::freeze(frontend.globals.globalTypes);
    for (const Luau::ParseError &error : loaded.parseResult.errors)
        add_issue(issues, std::string{definitions_name}, error.getLocation(), error.getMessage());
    if (loaded.module != nullptr)
        for (const Luau::TypeError &error : loaded.module->errors)
            add_issue(issues, std::string{definitions_name}, error.location, Luau::toString(error));
    return loaded.success;
}

} // namespace

std::vector<TypeIssue> check_definitions(std::string_view definitions)
{
    std::vector<TypeIssue> issues;
    Luau::NullFileResolver files;
    Luau::NullConfigResolver config;
    Luau::Frontend frontend{&files, &config};
    if (!prepare(frontend, definitions, issues) && issues.empty())
        issues.push_back({std::string{definitions_name}, 0, 0, "Luau refused the definitions"});
    return issues;
}

std::vector<TypeIssue> check_scripts(const std::map<std::string, std::string> &scripts, std::string_view definitions)
{
    std::vector<TypeIssue> issues;
    ModFiles files{scripts};
    Luau::NullConfigResolver config;
    Luau::Frontend frontend{&files, &config};
    if (!prepare(frontend, definitions, issues))
        return issues;
    for (const auto &[path, source] : scripts)
    {
        const Luau::CheckResult result = frontend.check(path);
        for (const Luau::TypeError &error : result.errors)
            if (error.moduleName == path)
                add_issue(issues, path, error.location, Luau::toString(error));
    }
    std::sort(issues.begin(), issues.end(), [](const TypeIssue &a, const TypeIssue &b) {
        return a.file != b.file ? a.file < b.file : a.line < b.line;
    });
    return issues;
}

} // namespace openminerz::mod_sdk
