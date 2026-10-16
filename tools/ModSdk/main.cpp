// openminerz_mod_sdk: the modding SDK's command-line tool.
//
//   openminerz_mod_sdk new <id> [--template client|server] [--dir <folder>]
//   openminerz_mod_sdk validate <mod folder>... [--strict] [--types]
//   openminerz_mod_sdk run <mod folder>... [--seconds N] [--chat "/command"]... [--classic]
//                          [--spawn mod:creature@x,y,z]... [--creatures]
//   openminerz_mod_sdk digest <mod folder>
//   openminerz_mod_sdk pack <mod folder> [--out <folder>]
//   openminerz_mod_sdk reference [--repo <source tree>] [--check]
//   openminerz_mod_sdk preview-map <mod folder>... [--seed N] [--size blocks] [--center x,z] [--out map.png]
//   openminerz_mod_sdk export-rig [--out file.glb] [--reference-clips ...]
//   openminerz_mod_sdk check-anim <file>... [--clips]
//
// Every command that needs the API builds it the way the game does
// (sdk_session.hpp), so what validate accepts and what the reference says is
// what the game will do.

#include "ai_tools.hpp" // mod-ai
#include "anim_commands.hpp"
#include "rig_lab.hpp"
#include "reference.hpp"
#include "sdk_session.hpp"
#include "templates.hpp"
#include "typecheck.hpp"
#include "validate.hpp"
#include "world_tools.hpp" // mod-world

#include "openminerz/modding/package.hpp"
#include "openminerz/modding/runtime.hpp"
#include "openminerz/net/mod_catalog.hpp"

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Lexer.h"
#include "Luau/ParseOptions.h"
#include "Luau/Parser.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using namespace openminerz;
using namespace openminerz::mod_sdk;

constexpr std::string_view usage = R"(OpenMinerZ mod SDK

  openminerz_mod_sdk new <id> [--template client|server] [--dir <folder>]
      Creates <folder>/<id>/ with a descriptor, scripts and a README to start from.
      client: UI, HUD, textures, lights; works on any server, vanilla included.
      server: gameplay; every player in the session runs it (joiners download it).

  openminerz_mod_sdk validate <mod folder>... [--strict] [--types]
      Checks what the game would refuse or trip over: descriptor, file types,
      sizes, Luau syntax and compile errors, unknown API names and events,
      server-only calls in client code (and the reverse), capabilities,
      require() paths and cycles, lang files and asset paths.
      --types adds Luau type-checker warnings against the API definitions.
      --strict also fails on warnings.

  openminerz_mod_sdk run <mod folder>... [--seconds N] [--chat "/command"]... [--classic]
                         [--spawn mod:creature@x,y,z]... [--creatures]
      Runs the mods headless in a flat test world (server VM, client VM, mod
      channel, HUD) for N seconds (default 10), typing each --chat line after
      the first second, and prints their log. Fails when a script logs an error.
      --classic plays by the 1.6.3.8 rules (its item set) instead of 1.9.9.8.
      --spawn places a creature after the first second (the player stands at
      8.5, 1, 8.5); --creatures prints the AI's load and every creature,
      projectile and boss bar the client sees at the end.

  openminerz_mod_sdk digest <mod folder>
      The content digest players' clients compare when joining, the
      client-visible digest and sizes.

  openminerz_mod_sdk pack <mod folder> [--out <folder>]
      Validates, then copies the mod into <out>/<id>-<version>/<id>/ ready to
      zip and share (default <out>: ./release). No archive is written: the game
      installs mods as folders.

  openminerz_mod_sdk reference [--repo <source tree>] [--check]
      Writes docs/modding/api-reference.md and tools/ModSdk/types/ from the
      registry; --check only compares and fails when they are stale.

  openminerz_mod_sdk preview-map <mod folder>... [--seed N] [--size blocks] [--center x,z]
                                 [--pixels N] [--classic] [--out map.png]
      Draws the world the mods' terrain.define makes, from above: each biome's
      colour, shaded by height, water below the sea level, the origin (black)
      and the spawn (red), with the same native generator the game runs.
      Defaults: --size 52000 (out to 26,000 blocks each way), --center 0,0,
      --pixels 1024, --out map.png. Without a world generator, retail's map.

  openminerz_mod_sdk export-rig [--out <file.glb>] [--classic|--modern|--rig <character.omrc>]
                                [--reference-clips all|<Clip,Clip,...>] [--no-proxy]
      Writes the player skeleton (71 bones, rest pose) with a generated box body,
      to animate in Blender, read from the content you imported from your own
      game (Modern first). --reference-clips adds the game's own clips as REF_
      animations, for timing reference only: never ship them in a mod.

  openminerz_mod_sdk export-rig-lab --rig <character.omrc> --out <folder> [--avatar <avatar.ocavatar>]
      Exports locally owned skins, bone mappings, influence masks and native
      skinning reference matrices for the local Rig Lab editor. Do not ship
      these owned meshes with a mod.

  openminerz_mod_sdk check-anim <file.glb|.gltf>... [--classic|--modern|--rig <file>]
                                [--clips] [--strict]
      What the game will do with an animation pack file: which clips it replaces,
      which bones it animates, unknown bone names (with suggestions), a missing
      Root, bad scale or axes, timing, and the game's own clips. --clips lists
      the clip names a pack may use.
)";

std::optional<std::string> read_file(const fs::path &path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string_view scope_text(net::ModScope scope) noexcept
{
    return scope == net::ModScope::client ? "client" : scope == net::ModScope::server ? "server" : "core";
}

bool write_file(const fs::path &path, std::string_view text)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

// The value after `--name`, or nullopt.
std::optional<std::string> option(std::vector<std::string> &args, std::string_view name)
{
    const auto found = std::find(args.begin(), args.end(), name);
    if (found == args.end() || std::next(found) == args.end())
        return std::nullopt;
    std::string value = *std::next(found);
    args.erase(found, std::next(found, 2));
    return value;
}

bool flag(std::vector<std::string> &args, std::string_view name)
{
    const auto found = std::find(args.begin(), args.end(), name);
    if (found == args.end())
        return false;
    args.erase(found);
    return true;
}

// A declaration file must parse with Luau's own parser in declaration mode,
// the way luau-lsp reads it.
bool definitions_parse(std::string_view source, std::string &error)
{
    Luau::Allocator allocator;
    Luau::AstNameTable names{allocator};
    Luau::ParseOptions options;
    options.allowDeclarationSyntax = true;
    const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator, options);
    if (parsed.errors.empty())
        return true;
    const Luau::ParseError &first = parsed.errors.front();
    error = std::format("openminerz.d.luau:{}:{}: {}", first.getLocation().begin.line + 1,
                        first.getLocation().begin.column + 1, first.getMessage());
    return false;
}

int command_reference(std::vector<std::string> args)
{
    const fs::path repo = option(args, "--repo").value_or(fs::current_path().string());
    const bool check = flag(args, "--check");
    Sandbox sandbox;
    const GeneratedFiles generated = generate_reference(sandbox.registry());
    std::string error;
    if (!definitions_parse(generated.luau, error))
    {
        std::cerr << "the generated Luau definitions do not parse: " << error << '\n';
        return 1;
    }
    // And Luau's analyzer (what luau-lsp runs) loads them without complaint.
    if (const std::vector<TypeIssue> issues = check_definitions(generated.luau); !issues.empty())
    {
        for (const TypeIssue &issue : issues)
            std::cerr << std::format("{}:{}:{}: {}\n", issue.file, issue.line, issue.column, issue.message);
        std::cerr << "the generated Luau definitions do not type-check\n";
        return 1;
    }
    const std::pair<fs::path, const std::string *> outputs[]{
        {repo / "docs" / "modding" / "api-reference.md", &generated.markdown},
        {repo / "tools" / "ModSdk" / "types" / "openminerz.d.luau", &generated.luau},
        {repo / "tools" / "ModSdk" / "types" / "openminerz-api.json", &generated.json},
    };
    int stale{};
    for (const auto &[path, text] : outputs)
    {
        const std::optional<std::string> current = read_file(path);
        // Git may check text out with CRLF line ends; compare the text.
        std::string normalized = current.value_or(std::string{});
        std::erase(normalized, '\r');
        const bool same = current.has_value() && normalized == *text;
        if (check)
        {
            if (!same)
            {
                ++stale;
                // The first line that differs, so a CI log says what moved.
                std::size_t line = 1U;
                std::size_t at = 0U;
                while (at < normalized.size() && at < text->size() && normalized[at] == (*text)[at])
                    line += normalized[at++] == '\n' ? 1U : 0U;
                const auto excerpt = [&](const std::string &from) {
                    const std::size_t begin = at == 0U ? std::string::npos : from.rfind('\n', at - 1U);
                    const std::size_t start = begin == std::string::npos ? 0U : begin + 1U;
                    const std::size_t end = from.find('\n', start);
                    return from.substr(start, std::min<std::size_t>(
                                                  (end == std::string::npos ? from.size() : end) - start, 160U));
                };
                std::cerr << "stale: " << path.generic_string() << " (from line " << line << ")\n"
                          << "  committed: " << (current ? excerpt(normalized) : std::string{"<missing>"}) << '\n'
                          << "  generated: " << excerpt(*text) << '\n';
            }
            continue;
        }
        if (same)
        {
            std::cout << "unchanged " << path.generic_string() << '\n';
            continue;
        }
        if (!write_file(path, *text))
        {
            std::cerr << "cannot write " << path.generic_string() << '\n';
            return 1;
        }
        std::cout << "wrote " << path.generic_string() << '\n';
    }
    if (stale != 0)
    {
        std::cerr << "The API changed: run `openminerz_mod_sdk reference --repo <source tree>` and commit "
                     "the regenerated files.\n";
        return 1;
    }
    if (check)
        std::cout << "API reference, Luau definitions and JSON are current ("
                  << sandbox.registry().functions().size() << " functions, " << sandbox.registry().events().size()
                  << " events).\n";
    return 0;
}

// `validate --types`: Luau's type checker over the mod's scripts, against
// the generated definitions, as warnings (the API takes plain tables in many
// places, so a type complaint is advice, not a refusal).
void add_type_warnings(ValidationResult &result, const std::string &definitions)
{
    std::vector<net::ModContentFile> files;
    std::string error;
    if (!net::list_mod_content(result.mod.root, files, error))
        return;
    std::map<std::string, std::string> scripts;
    for (const net::ModContentFile &file : files)
        if ((file.path.ends_with(".luau") || file.path.ends_with(".lua")) &&
            (file.path.starts_with("shared/") || file.path.starts_with("client/") ||
             file.path.starts_with("server/")))
            if (const std::optional<std::string> text = read_file(file.source))
                scripts.emplace(file.path, *text);
    for (const TypeIssue &issue : check_scripts(scripts, definitions))
        result.diagnostics.push_back(
            {Diagnostic::Level::warning, issue.file, issue.line, issue.column, "type: " + issue.message});
}

int command_validate(std::vector<std::string> args)
{
    const bool strict = flag(args, "--strict");
    const bool types = flag(args, "--types");
    if (args.empty())
    {
        std::cerr << "validate: name at least one mod folder\n";
        return 2;
    }
    Sandbox sandbox;
    int failed{};
    for (const std::string &folder : args)
    {
        ValidationResult result = validate_mod(folder, sandbox.registry());
        if (types && result.loaded)
            add_type_warnings(result, generate_reference(sandbox.registry()).luau);
        std::cout << format_diagnostics(result);
        const std::size_t errors = result.count(Diagnostic::Level::error);
        const std::size_t warnings = result.count(Diagnostic::Level::warning);
        const bool ok = errors == 0U && result.loaded && (!strict || warnings == 0U);
        if (result.loaded)
            std::cout << std::format("{} {} ({} mod): {} files, {} scripts, {:.1f} KiB; {} errors, {} warnings: {}\n",
                                     result.mod.descriptor.id, result.mod.descriptor.version,
                                     scope_text(result.mod.descriptor.scope), result.files, result.scripts,
                                     static_cast<double>(result.bytes) / 1024.0, errors, warnings,
                                     ok ? "OK" : "FAILED");
        else
            std::cout << folder << ": FAILED\n";
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}

int command_digest(std::vector<std::string> args)
{
    if (args.size() != 1U)
    {
        std::cerr << "digest: name one mod folder\n";
        return 2;
    }
    net::LoadedMod mod;
    std::string error;
    if (!load_mod_folder(args.front(), mod, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    net::ModDigest client{};
    std::uint64_t client_bytes{};
    if (!net::client_visible_digest(mod.root, client, client_bytes, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    std::vector<net::ModContentFile> files;
    std::uint64_t bytes{};
    if (net::list_mod_content(mod.root, files, error))
        for (const net::ModContentFile &file : files)
            bytes += file.size;
    std::cout << std::format("id       {}\nversion  {}\nscope    {}\nprotocol {}\n", mod.descriptor.id,
                             mod.descriptor.version, scope_text(mod.descriptor.scope),
                             mod.classic && mod.modern ? "both" : mod.classic ? "classic" : "modern");
    std::cout << std::format("content  {}  ({} files, {} bytes)\n", net::digest_hex(mod.descriptor.content_digest),
                             files.size(), bytes);
    std::cout << std::format("client   {}  ({} bytes a joining player downloads)\n", net::digest_hex(client),
                             mod.descriptor.scope == net::ModScope::client ? 0U : client_bytes);
    return 0;
}

int command_pack(std::vector<std::string> args)
{
    const fs::path out = option(args, "--out").value_or("release");
    if (args.size() != 1U)
    {
        std::cerr << "pack: name one mod folder\n";
        return 2;
    }
    Sandbox sandbox;
    const ValidationResult result = validate_mod(args.front(), sandbox.registry());
    std::cout << format_diagnostics(result);
    if (!result.ok())
    {
        std::cerr << "pack: fix the errors above first\n";
        return 1;
    }
    std::vector<net::ModContentFile> files;
    std::string error;
    if (!net::list_mod_content(result.mod.root, files, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    const std::string release = std::format("{}-{}", result.mod.descriptor.id, result.mod.descriptor.version);
    const fs::path target = out / release / result.mod.descriptor.id;
    std::error_code fs_error;
    if (fs::exists(target, fs_error))
    {
        std::cerr << "pack: " << target.generic_string() << " already exists (bump the version or remove it)\n";
        return 1;
    }
    for (const net::ModContentFile &file : files)
    {
        const fs::path destination = target / fs::path{file.source}.lexically_relative(result.mod.root);
        fs::create_directories(destination.parent_path(), fs_error);
        if (!fs::copy_file(file.source, destination, fs_error))
        {
            std::cerr << "pack: cannot copy " << file.path << ": " << fs_error.message() << '\n';
            return 1;
        }
    }
    // The copy must be byte-for-byte the mod players will compare.
    net::LoadedMod copied;
    if (!load_mod_folder(target, copied, error) || copied.descriptor.content_digest != result.mod.descriptor.content_digest)
    {
        std::cerr << "pack: the copy does not match the original (" << error << ")\n";
        return 1;
    }
    const std::string notes = std::format(
        "{} {} ({} mod)\ncontent digest {}\nclient digest  {}\n\nInstall: copy the folder '{}' into "
        "%LOCALAPPDATA%\\OpenMinerZ\\Mods\\ (or addons\\ next to the game).\n",
        result.mod.descriptor.id, result.mod.descriptor.version, scope_text(result.mod.descriptor.scope),
        result.digest, result.client_digest, result.mod.descriptor.id);
    write_file(out / release / "RELEASE.txt", notes);
    std::cout << "packed " << target.generic_string() << '\n' << notes;
    return 0;
}

// mod-world: a top-down map of the world the mods define.
int command_preview_map(std::vector<std::string> args)
{
    WorldMapRequest request;
    const fs::path out = option(args, "--out").value_or("map.png");
    std::int32_t seed = 20261014;
    if (const std::optional<std::string> value = option(args, "--seed"))
        seed = static_cast<std::int32_t>(std::strtoll(value->c_str(), nullptr, 10));
    if (const std::optional<std::string> value = option(args, "--size"))
        request.size = std::clamp(std::atof(value->c_str()), 16.0, 4.0e6);
    if (const std::optional<std::string> value = option(args, "--pixels"))
        request.pixels = std::clamp(std::atoi(value->c_str()), 16, 8192);
    if (const std::optional<std::string> value = option(args, "--center"))
    {
        const auto comma = value->find(',');
        if (comma == std::string::npos)
        {
            std::cerr << "preview-map: --center is x,z\n";
            return 2;
        }
        request.center_x = std::atof(value->substr(0U, comma).c_str());
        request.center_z = std::atof(value->substr(comma + 1U).c_str());
    }
    const bool classic = flag(args, "--classic");
    if (args.empty())
    {
        std::cerr << "preview-map: name at least one mod folder\n";
        return 2;
    }
    std::vector<net::LoadedMod> mods;
    for (const std::string &folder : args)
    {
        net::LoadedMod mod;
        std::string error;
        if (!load_mod_folder(folder, mod, error))
        {
            std::cerr << folder << ": " << error << '\n';
            return 1;
        }
        mods.push_back(std::move(mod));
    }
    std::stable_sort(mods.begin(), mods.end(), [](const net::LoadedMod &a, const net::LoadedMod &b) {
        return a.load_order < b.load_order;
    });
    Sandbox sandbox;
    sandbox.world().seed = seed;
    sandbox.world().modern = !classic;
    std::string error;
    if (!sandbox.start(mods, error))
    {
        std::cerr << "preview-map: " << error << '\n';
        return 1;
    }
    for (const modding::LogEntry &entry : sandbox.log())
        if (entry.level == modding::LogLevel::error)
            std::cerr << entry.mod_id << ": " << entry.message << '\n';
    if (sandbox.terrain() == nullptr)
        std::cout << "No mod defines a world (terrain.define): drawing retail terrain.\n";
    else
        std::cout << std::format("World of {} ({} biomes, digest {}), seed {}\n",
                                 sandbox.terrain()->definition().mod_id,
                                 sandbox.terrain()->definition().biomes.size(),
                                 sandbox.terrain()->digest(), seed);
    // The registry reaches the client half over the loopback channel.
    sandbox.run(0.1);
    request.palette_colors = palette_colors(sandbox);
    const auto started = std::chrono::steady_clock::now();
    const WorldMap map = render_world_map(sandbox.terrain_generator(), request);
    if (!write_png(out, map.pixels, map.pixels, map.rgb))
    {
        std::cerr << "preview-map: cannot write " << out.generic_string() << '\n';
        return 1;
    }
    std::cout << std::format("{} x {} blocks around {},{} -> {} ({} px, {:.1f} s)\n", request.size, request.size,
                             request.center_x, request.center_z, out.generic_string(), map.pixels,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    for (const std::string &line : map.legend)
        std::cout << "  " << line << '\n';
    return 0;
}

int command_run(std::vector<std::string> args)
{
    double seconds = 10.0;
    if (const std::optional<std::string> value = option(args, "--seconds"))
        seconds = std::clamp(std::atof(value->c_str()), 0.0, 3600.0);
    std::vector<std::string> chat;
    while (const std::optional<std::string> line = option(args, "--chat"))
        chat.push_back(*line);
    // mod-ai: creatures to place, and the AI summary at the end.
    std::vector<std::string> spawns;
    while (const std::optional<std::string> spawn = option(args, "--spawn"))
        spawns.push_back(*spawn);
    const bool creatures = flag(args, "--creatures");
    const bool quiet = flag(args, "--quiet");
    const bool classic = flag(args, "--classic");
    if (args.empty())
    {
        std::cerr << "run: name at least one mod folder\n";
        return 2;
    }
    std::vector<net::LoadedMod> mods;
    for (const std::string &folder : args)
    {
        net::LoadedMod mod;
        std::string error;
        if (!load_mod_folder(folder, mod, error))
        {
            std::cerr << folder << ": " << error << '\n';
            return 1;
        }
        mods.push_back(std::move(mod));
    }
    std::stable_sort(mods.begin(), mods.end(), [](const net::LoadedMod &a, const net::LoadedMod &b) {
        return a.load_order < b.load_order;
    });
    Sandbox sandbox;
    sandbox.set_echo(!quiet);
    sandbox.world().modern = !classic;
    std::string error;
    if (!sandbox.start(mods, error))
    {
        std::cerr << "run: " << error << '\n';
        return 1;
    }
    for (const auto &[id, reason] : sandbox.refused())
        std::cerr << "not loaded: " << id << ": " << reason << '\n';
    sandbox.run(std::min(seconds, 1.0));
    for (const std::string &spawn : spawns)
    {
        // "mod:kind@x,y,z" (position optional: 6 blocks east of the player).
        const auto at = spawn.find('@');
        const std::string kind = spawn.substr(0U, at);
        std::string where = "vector.create(14.5, 1, 8.5)";
        if (at != std::string::npos)
            where = std::format("vector.create({})", spawn.substr(at + 1U));
        const modding::RunResult placed = sandbox.runtime().run(
            modding::Side::server, mods.front().descriptor.id, "sdk-spawn",
            std::format("return entities.spawn(\"{}\", {})", kind, where));
        std::cout << "spawn " << spawn << ": " << (placed.ok && !placed.value.is_nil() ? "placed" : placed.error)
                  << '\n';
    }
    for (const std::string &line : chat)
    {
        const modding::gameplay::ChatVerdict verdict = sandbox.chat(line);
        std::cout << "chat \"" << line << "\": "
                  << (verdict == modding::gameplay::ChatVerdict::command  ? "handled by a mod command"
                      : verdict == modding::gameplay::ChatVerdict::cancel ? "cancelled by a mod"
                                                                          : "passed through")
                  << '\n';
        for (const std::string &shown : sandbox.world().chat_lines)
            std::cout << "  chat> " << shown << '\n';
        sandbox.world().chat_lines.clear();
    }
    if (seconds > 1.0)
        sandbox.run(seconds - 1.0);
    if (creatures)
        std::cout << creature_summary(sandbox);
    const SandboxReport report = sandbox.report();
    for (const modding::ModStatus &status : sandbox.runtime().status())
        std::cout << std::format("{} [{}] {}{}{}\n", status.id, modding::side_name(status.side),
                                 status.state == modding::ModState::running  ? "running"
                                 : status.state == modding::ModState::failed ? "FAILED"
                                                                              : "STOPPED",
                                 status.error.empty() ? "" : ": ", status.error);
    std::cout << std::format("{:.1f} s simulated: {} errors, {} warnings; HUD {} draw commands, screens {}; "
                             "{} lights, {} atmosphere layers, {} models, {} creatures; player health {:.0f}\n",
                             seconds, report.errors, report.warnings, report.hud_commands, report.screen_commands,
                             report.lights, report.atmosphere_layers, report.models, report.entities,
                             sandbox.world().health);
    sandbox.stop();
    const bool failed = std::any_of(sandbox.runtime().status().begin(), sandbox.runtime().status().end(),
                                    [](const modding::ModStatus &status) {
                                        return status.state != modding::ModState::running;
                                    });
    return report.errors == 0U && !failed ? 0 : 1;
}

int command_new(std::vector<std::string> args)
{
    const std::string kind = option(args, "--template").value_or("server");
    const fs::path parent = option(args, "--dir").value_or(fs::current_path().string());
    if (args.size() != 1U)
    {
        std::cerr << "new: name the mod id (letters, digits, _ and -)\n";
        return 2;
    }
    const std::string &id = args.front();
    if (id.empty() || id.size() > 64U ||
        !std::all_of(id.begin(), id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }))
    {
        std::cerr << "new: ids are 1-64 lowercase letters, digits, _ or -\n";
        return 2;
    }
    const std::vector<TemplateFile> *files = template_files(kind);
    if (files == nullptr)
    {
        std::cerr << "new: --template is client or server\n";
        return 2;
    }
    const fs::path root = parent / id;
    std::error_code error;
    if (fs::exists(root, error))
    {
        std::cerr << "new: " << root.generic_string() << " already exists\n";
        return 1;
    }
    for (const TemplateFile &file : *files)
    {
        std::string text{file.text};
        for (std::size_t at = text.find("{{id}}"); at != std::string::npos; at = text.find("{{id}}", at))
            text.replace(at, 6U, id);
        if (!write_file(root / file.path, text))
        {
            std::cerr << "new: cannot write " << (root / file.path).generic_string() << '\n';
            return 1;
        }
    }
    std::cout << std::format("Created {} ({} mod). Next:\n  openminerz_mod_sdk validate \"{}\"\n"
                             "  openminerz_mod_sdk run \"{}\"\nthen copy the folder into %LOCALAPPDATA%\\OpenMinerZ\\Mods\\.\n",
                             root.generic_string(), kind, root.generic_string(), root.generic_string());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> args{argv + std::min(argc, 1), argv + argc};
    if (args.empty() || args.front() == "--help" || args.front() == "-h" || args.front() == "help")
    {
        std::cout << usage;
        return args.empty() ? 2 : 0;
    }
    const std::string command = args.front();
    args.erase(args.begin());
    try
    {
        if (command == "reference")
            return command_reference(std::move(args));
        if (command == "validate")
            return command_validate(std::move(args));
        if (command == "digest")
            return command_digest(std::move(args));
        if (command == "pack")
            return command_pack(std::move(args));
        if (command == "run")
            return command_run(std::move(args));
        if (command == "preview-map")
            return command_preview_map(std::move(args));
        if (command == "new")
            return command_new(std::move(args));
        if (command == "export-rig")
            return command_export_rig(std::move(args));
        if (command == "export-rig-lab")
            return command_export_rig_lab(std::move(args));
        if (command == "check-anim")
            return command_check_anim(std::move(args));
    }
    catch (const std::exception &error)
    {
        std::cerr << command << ": " << error.what() << '\n';
        return 1;
    }
    std::cerr << "unknown command '" << command << "'\n\n" << usage;
    return 2;
}
