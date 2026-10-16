#pragma once

// The starting points `openminerz_mod_sdk new` writes. "{{id}}" in a file is
// replaced by the new mod's id. Each template validates cleanly and runs in the
// sandbox (tests/test_mod_examples.cpp checks both).

#include <string_view>
#include <vector>

namespace openminerz::mod_sdk
{

struct TemplateFile final
{
    std::string_view path; // mod-relative
    std::string_view text;
};

// "client" or "server"; nullptr for anything else.
[[nodiscard]] const std::vector<TemplateFile> *template_files(std::string_view kind);

} // namespace openminerz::mod_sdk
