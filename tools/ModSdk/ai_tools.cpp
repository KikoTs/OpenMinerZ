// mod-ai: the SDK's creature AI tools (ai_tools.hpp).

#include "ai_tools.hpp"

#include "sdk_session.hpp"

#include "openminerz/modding/gameplay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>

namespace openminerz::mod_sdk
{

void check_creature_ai(ValidationResult &result)
{
    if (!result.ok())
        return;
    // Only a mod whose scripts use the AI.
    static constexpr std::array<std::string_view, 5> markers{"creatures.define", "brains.define",
                                                             "projectiles.define", "enemies.rules", "ai."};
    bool uses{};
    std::error_code error_code;
    for (const char *folder : {"shared", "server"})
    {
        const std::filesystem::path root = result.mod.root / folder;
        if (!std::filesystem::is_directory(root, error_code))
            continue;
        for (const auto &entry : std::filesystem::recursive_directory_iterator{root, error_code})
        {
            if (!entry.is_regular_file() ||
                (entry.path().extension() != ".luau" && entry.path().extension() != ".lua"))
                continue;
            std::ifstream input{entry.path(), std::ios::binary};
            const std::string text{std::istreambuf_iterator<char>{input}, {}};
            uses = uses || std::ranges::any_of(markers, [&](std::string_view marker) {
                       return text.find(marker) != std::string::npos;
                   });
        }
    }
    if (!uses)
        return;
    std::vector<Diagnostic> &out = result.diagnostics;
    Sandbox sandbox;
    std::string error;
    const std::vector<net::LoadedMod> mods{result.mod};
    if (!sandbox.start(mods, error))
    {
        out.push_back({Diagnostic::Level::warning, "", 0, 0, "the creatures could not be loaded in the sandbox: " + error});
        return;
    }
    // Definitions are checked as they are made: a bad field is a script error.
    for (const modding::LogEntry &entry : sandbox.log())
        if (entry.level == modding::LogLevel::error)
            out.push_back({Diagnostic::Level::error, "", 0, 0, entry.message});
    for (const std::string &problem : sandbox.gameplay().ai_definition_problems())
        out.push_back({Diagnostic::Level::error, "", 0, 0, problem});
    sandbox.stop();
}

std::string creature_summary(Sandbox &sandbox)
{
    modding::gameplay::GameplayService &service = sandbox.gameplay();
    std::string text = service.ai_report() + '\n';
    const modding::Vec3 me = sandbox.world().position;
    for (const modding::gameplay::ClientEntity &entity : service.client_entities())
        text += std::format("  {} #{} at ({:.1f}, {:.1f}, {:.1f}) {:.0f} blocks off, {} health {:.0f}%{}\n",
                            entity.kind, entity.id, entity.position.x, entity.position.y, entity.position.z,
                            std::hypot(entity.position.x - me.x, entity.position.z - me.z), entity.animation,
                            entity.health_fraction * 100.0F, entity.name.empty() ? "" : " \"" + entity.name + "\"");
    for (const modding::gameplay::ClientProjectile &projectile : service.client_projectiles())
        text += std::format("  projectile {} at ({:.1f}, {:.1f}, {:.1f})\n", projectile.kind, projectile.position.x,
                            projectile.position.y, projectile.position.z);
    for (const modding::gameplay::ClientBoss &boss : service.client_bosses())
        text += std::format("  boss bar \"{}\" {:.0f}%{}{}\n", boss.title, boss.fraction * 100.0F,
                            boss.phase.empty() ? "" : " - " + boss.phase,
                            boss.music.empty() ? "" : " (music " + boss.music + ")");
    const modding::gameplay::GameplayService &read = service;
    if (read.retail_rules_active())
        text += "  retail enemy rules are on (enemies.rules)\n";
    return text;
}

} // namespace openminerz::mod_sdk
