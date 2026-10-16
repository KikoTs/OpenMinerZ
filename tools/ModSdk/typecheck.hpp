#pragma once

// Luau's own type checker (Luau.Analysis, the engine luau-lsp runs) over the
// generated definitions: proves openminerz.d.luau loads the way an editor
// loads it, and checks a mod's scripts against it for `validate --types`.

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::mod_sdk
{

struct TypeIssue final
{
    std::string file; // mod-relative path, or "openminerz.d.luau"
    int line{};       // 1-based
    int column{};
    std::string message;
};

// Every problem Luau reports while loading `definitions` as a declaration
// file on top of its builtins. Empty: an editor loads it cleanly.
[[nodiscard]] std::vector<TypeIssue> check_definitions(std::string_view definitions);

// Type-checks scripts (mod-relative path -> source) against `definitions`,
// in Luau's non-strict mode unless a file asks for --!strict. require() of
// another script of the mod resolves with the runtime's rules.
[[nodiscard]] std::vector<TypeIssue> check_scripts(const std::map<std::string, std::string> &scripts,
                                                   std::string_view definitions);

} // namespace openminerz::mod_sdk
