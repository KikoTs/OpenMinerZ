// `openminerz_mod_sdk export-rig` and `check-anim` (anim_commands.hpp).
//
// Both read the player character exactly where the game does: the content
// pack imported from the user's own CastleMiner Z (DerivedData/character.omrc,
// found through openminerz/asset_paths.hpp). Nothing of the game is built into
// the tool.

#include "anim_commands.hpp"

#include "openminerz/asset_paths.hpp"
#include "openminerz/modding/animation_authoring.hpp"
#include "openminerz/retail_rig.hpp"
#include "openminerz/world/player_animation.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openminerz::mod_sdk
{
namespace
{

namespace fs = std::filesystem;
namespace graphics = modding::graphics;

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

struct LoadedRig final
{
    std::string label;
    fs::path path;
    std::unique_ptr<rig::RetailRig> rig;
};

std::string rig_label(const rig::RetailRig &rig)
{
    std::string generation = rig.generation.empty() ? std::string{"Retail"} : rig.generation;
    generation[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(generation[0])));
    return rig.game_version.empty() ? generation : generation + " " + rig.game_version;
}

constexpr std::string_view missing_rig_help =
    "No imported player character was found. The SDK reads the skeleton from the game you own, where "
    "OpenMinerZ reads it: import your CastleMiner Z content first (start OpenMinerZ once with the game "
    "installed, or run import-assets.bat), or set OPENMINERZ_CONTENT_PACKS to a folder "
    "holding classic/ and modern/ content packs, or pass --rig <pack>/DerivedData/character.omrc.";

// --rig FILE, or the Classic and/or Modern pack (--classic, --modern; both by
// default). `first_only` stops at the first found, Modern first.
std::vector<LoadedRig> load_rigs(std::vector<std::string> &args, bool first_only, std::string &problems)
{
    std::vector<LoadedRig> rigs;
    const auto load = [&](const fs::path &path) {
        try
        {
            auto loaded = std::make_unique<rig::RetailRig>(path);
            const std::string label = rig_label(*loaded);
            rigs.push_back({label, path, std::move(loaded)});
        }
        catch (const std::exception &error)
        {
            problems += std::format("{}: {}\n", path.generic_string(), error.what());
        }
    };
    const std::optional<std::string> file = option(args, "--rig");
    bool classic = flag(args, "--classic");
    bool modern = flag(args, "--modern");
    if (file)
    {
        load(*file);
        return rigs;
    }
    if (!classic && !modern)
        classic = modern = true;
    for (const AssetStyle style : {AssetStyle::modern, AssetStyle::classic})
    {
        if ((style == AssetStyle::modern && !modern) || (style == AssetStyle::classic && !classic))
            continue;
        if (first_only && !rigs.empty())
            break;
        const fs::path path = content_pack_path(style) / "DerivedData" / "character.omrc";
        std::error_code error;
        if (fs::is_regular_file(path, error))
            load(path);
    }
    return rigs;
}

std::vector<std::string> split_names(std::string_view text)
{
    std::vector<std::string> names;
    std::size_t start{};
    while (start <= text.size())
    {
        std::size_t end = text.find(',', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string name{text.substr(start, end - start)};
        std::erase(name, ' ');
        if (!name.empty())
            names.push_back(std::move(name));
        start = end + 1U;
    }
    return names;
}

std::string joined(const std::vector<std::string> &items)
{
    std::string text;
    for (const std::string &item : items)
        text += (text.empty() ? "" : ", ") + item;
    return text;
}

} // namespace

int command_export_rig(std::vector<std::string> args)
{
    const std::optional<std::string> out = option(args, "--out");
    const std::optional<std::string> references = option(args, "--reference-clips");
    const bool no_proxy = flag(args, "--no-proxy");
    std::string problems;
    std::vector<LoadedRig> rigs = load_rigs(args, true, problems);
    if (!args.empty())
    {
        std::cerr << "export-rig: unexpected '" << args.front() << "'\n";
        return 2;
    }
    if (rigs.empty())
    {
        std::cerr << problems << missing_rig_help << '\n';
        return 1;
    }
    const LoadedRig &loaded = rigs.front();
    graphics::RigExportOptions options;
    options.proxy_mesh = !no_proxy;
    if (references)
    {
        if (*references == "all")
            for (const rig::Registration &registration : loaded.rig->registrations)
                options.reference_clips.push_back(registration.name);
        else
            options.reference_clips = split_names(*references);
    }
    std::string error;
    const std::vector<std::uint8_t> bytes = graphics::export_rig(*loaded.rig, options, &error);
    if (bytes.empty())
    {
        std::cerr << "export-rig: " << error << " (`check-anim --clips` lists the names)\n";
        return 1;
    }
    const fs::path path =
        out ? fs::path{*out}
            : fs::path{std::format("openminerz-rig-{}{}.glb", loaded.rig->generation.empty() ? "retail" : loaded.rig->generation,
                                   options.reference_clips.empty() ? "" : "-reference")};
    std::error_code fs_error;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), fs_error);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
    {
        std::cerr << "export-rig: cannot write " << path.generic_string() << '\n';
        return 1;
    }
    std::cout << std::format(
        "Wrote {} ({:.0f} KiB): the player skeleton of {} ({} bones){}, {} reference clip(s).\n"
        "  Read from {}\n"
        "  Axes: +Y up, the character faces +Z, its left hand on +X; one unit is one block (1 m in Blender).\n"
        "  Blender: set the scene to 30 fps, then File > Import > glTF 2.0 with the default settings.\n"
        "  These 71 bones also drive Xbox avatar skins: a pack made on them animates avatars too.\n",
        path.generic_string(), static_cast<double>(bytes.size()) / 1024.0, loaded.label, loaded.rig->attachment.size(),
        options.proxy_mesh ? " and a generated proxy body" : "", options.reference_clips.size(),
        loaded.path.generic_string());
    if (!options.reference_clips.empty())
        std::cout << "\nNOTICE: the REF_ animations in this file are CastleMiner Z's own animations, read from\n"
                     "your installed game for local timing reference only. Do not ship them, or edited copies\n"
                     "of them, in a mod: the game ignores REF_ clips, and `validate` and `check-anim` refuse a\n"
                     "pack that contains one. Animate your own clips on the skeleton.\n";
    return 0;
}

int command_check_anim(std::vector<std::string> args)
{
    const bool strict = flag(args, "--strict");
    const bool list = flag(args, "--clips");
    std::string problems;
    std::vector<LoadedRig> rigs = load_rigs(args, false, problems);
    std::vector<graphics::AnimationCheckRig> targets;
    for (const LoadedRig &loaded : rigs)
        targets.push_back({loaded.label, loaded.rig.get()});
    if (!problems.empty())
        std::cerr << problems;
    if (rigs.empty())
        std::cout << "note: no imported player character found, so only bone and clip names are checked (not the "
                     "hierarchy, scale, timing or a match with the game's own clips). Import your game's content "
                     "or pass --rig <pack>/DerivedData/character.omrc.\n";
    else
        std::cout << "Checking against " << [&] {
            std::vector<std::string> labels;
            for (const LoadedRig &loaded : rigs)
                labels.push_back(loaded.label);
            return joined(labels);
        }() << ".\n";

    if (list)
    {
        std::cout << "Clips a pack may replace (name the file or the animation after one):\n";
        for (const std::string &name : graphics::registered_clip_names(targets))
        {
            std::string line = std::format("  {:<26}", name);
            if (rigs.empty())
            {
                const world::AnimationClipInfo &info =
                    world::animation_clip_info(world::animation_clip_from_name(name));
                line += std::format("{:>8}  {:>8}  {}", info.classic_ticks > 0
                                                         ? std::format("{:.3f} s", rig::seconds_from_ticks(info.classic_ticks))
                                                         : std::string{"-"},
                                    info.modern_ticks > 0 ? std::format("{:.3f} s", rig::seconds_from_ticks(info.modern_ticks))
                                                          : std::string{"-"},
                                    info.looping ? "looping" : "once");
            }
            else
            {
                const rig::Registration *shown = nullptr;
                const rig::RetailRig *owner = nullptr;
                for (const LoadedRig &loaded : rigs)
                {
                    const rig::Registration *registration = loaded.rig->find(name);
                    if (registration == nullptr)
                    {
                        line += std::format("{:>9}", "-");
                        continue;
                    }
                    line += std::format("{:>9}", std::format("{:.3f} s", rig::seconds_from_ticks(
                                                                           loaded.rig->clips[static_cast<std::size_t>(registration->clip)].duration_ticks)));
                    if (shown == nullptr)
                    {
                        shown = registration;
                        owner = loaded.rig.get();
                    }
                }
                if (shown != nullptr)
                {
                    std::vector<std::string> roots;
                    for (std::size_t bone = 0U; bone < shown->influence.size(); ++bone)
                    {
                        const int parent = owner->attachment[bone].parent;
                        if (shown->influence[bone] && (parent < 0 || !shown->influence[static_cast<std::size_t>(parent)]))
                            roots.push_back(owner->attachment[bone].name);
                    }
                    line += std::format("  {:<8} moves {} and below", shown->looping ? "looping" : "once", joined(roots));
                }
            }
            std::cout << line << '\n';
        }
        if (rigs.empty())
            std::cout << "  (durations: Classic 1.6.3.8, Modern 1.9.9.8)\n";
        else
        {
            std::string order;
            for (const LoadedRig &loaded : rigs)
                order += (order.empty() ? "" : ", ") + loaded.label;
            std::cout << "  (durations: " << order << ")\n";
        }
    }
    if (args.empty())
    {
        if (list)
            return 0;
        std::cerr << "check-anim: name at least one .glb or .gltf file\n";
        return 2;
    }

    int failed{};
    for (const std::string &name : args)
    {
        const fs::path path{name};
        graphics::ModelLimits limits = graphics::animation_file_limits();
        // A rig exported with every reference clip is bigger than a pack.
        limits.max_animations = 1024U;
        limits.max_keys = 16U * 1024U * 1024U;
        std::string error;
        const auto model = graphics::load_model(path, &error, limits);
        std::cout << '\n' << path.generic_string() << '\n';
        if (model == nullptr)
        {
            std::cout << "  error: " << error << "\n  FAILED\n";
            ++failed;
            continue;
        }
        const graphics::AnimationCheck check = graphics::check_animation(*model, path.stem().string(), targets);
        for (const graphics::AnimationCheck::Clip &clip : check.clips)
        {
            const bool reference = clip.animation.starts_with(graphics::reference_clip_prefix);
            std::cout << std::format("  \"{}\" ({:.3f} s) {}\n", clip.animation, clip.seconds,
                                     reference                   ? std::string{"is the game's own clip: ignored"}
                                     : clip.registration.empty() ? std::string{"replaces nothing"}
                                                                 : "replaces " + clip.registration);
            if (clip.rotated.size() == static_cast<std::size_t>(rig::attachment_bone_count))
                std::cout << "    rotates all 71 bones\n";
            else if (!clip.rotated.empty())
                std::cout << std::format("    rotates {} bone(s): {}\n", clip.rotated.size(), joined(clip.rotated));
            if (clip.moved.size() == static_cast<std::size_t>(rig::attachment_bone_count))
                std::cout << "    moves all 71 bones (only the Root's translation is used)\n";
            else if (!clip.moved.empty())
                std::cout << std::format("    moves: {}\n", joined(clip.moved));
            if (!clip.scaled.empty())
                std::cout << std::format("    scales: {}\n", joined(clip.scaled));
            for (const std::string &line : clip.timing)
                std::cout << "    " << line << '\n';
        }
        for (const graphics::AnimationCheck::Message &message : check.messages)
            std::cout << "  "
                      << (message.level == graphics::AnimationCheck::Level::error     ? "error: "
                          : message.level == graphics::AnimationCheck::Level::warning ? "warning: "
                                                                                       : "note: ")
                      << message.text << '\n';
        const std::size_t errors = check.count(graphics::AnimationCheck::Level::error);
        const std::size_t warnings = check.count(graphics::AnimationCheck::Level::warning);
        const bool ok = errors == 0U && (!strict || warnings == 0U);
        std::cout << std::format("  {} error(s), {} warning(s): {}\n", errors, warnings, ok ? "OK" : "FAILED");
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}

} // namespace openminerz::mod_sdk
